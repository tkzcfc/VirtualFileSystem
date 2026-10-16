#include "ArchiveWriter.h"
#include "ArchiveReader.h"
#include "ByteIO.h"
#include "Compression.h"
#include "../Path.h"
#include "../crypto/ChaCha20.h"
#include "../crypto/Hash.h"
#include "../crypto/XorCipher.h"
#include "../native/NativeFileStream.h"
#include "../native/NativeUtils.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>

NS_VFS_BEGIN

using namespace ArchiveFormat;

namespace
{

// 派生用的种子常量。改动它们会改变输出字节，属于格式行为的一部分，不要随意修改
constexpr uint64_t SEED_FILE_KEY = 0x5646532D4B455931ull;    // "VFS-KEY1"
constexpr uint64_t SEED_INDEX_KEY = 0x5646532D49445831ull;   // "VFS-IDX1"
constexpr uint64_t SEED_INDEX_NONCE = 0x5646532D4E4F4E31ull; // "VFS-NON1"

constexpr size_t IO_BUFFER_SIZE = 1u << 20;   // 1 MB 流式缓冲

// 内容去重的键：内容哈希 + 长度 + 存储方式。XOR key 由内容哈希派生，所以相同内容的 key 也相同
struct DedupKey
{
	Hash128 hash;
	uint64_t size;
	uint8_t cipher;
	uint8_t compression;
	uint8_t keyLength;

	bool operator<(const DedupKey& o) const
	{
		if (hash != o.hash)
			return hash < o.hash;
		if (size != o.size)
			return size < o.size;
		if (cipher != o.cipher)
			return cipher < o.cipher;
		if (compression != o.compression)
			return compression < o.compression;
		return keyLength < o.keyLength;
	}
};

struct DedupValue
{
	uint64_t offset;
	uint64_t storedSize;
};

// 由内容哈希确定性派生 keyLen 字节的 XOR key（不随机，保证可复现）
void deriveFileKey(const Hash128& contentHash, uint8_t* key, uint32_t keyLen)
{
	uint8_t seedBytes[16];
	ByteIO::writeU64LE(seedBytes, contentHash.low);
	ByteIO::writeU64LE(seedBytes + 8, contentHash.high);
	for (uint32_t i = 0; i < keyLen; i += 16)
	{
		Hash128 block = xxh3_128(seedBytes, sizeof(seedBytes), SEED_FILE_KEY + i);
		uint8_t bytes[16];
		ByteIO::writeU64LE(bytes, block.low);
		ByteIO::writeU64LE(bytes + 8, block.high);
		std::memcpy(key + i, bytes, std::min<uint32_t>(16, keyLen - i));
	}
}

uint64_t alignUp(uint64_t v, uint32_t alignment)
{
	return (v + alignment - 1) / alignment * alignment;
}

// 写满 size 字节
Error writeAll(IFileStream& out, const uint8_t* data, size_t size)
{
	while (size > 0)
	{
		unsigned int chunk = static_cast<unsigned int>(std::min<size_t>(size, VFS_MAX_IO_CHUNK));
		int n = out.write(data, chunk);
		if (n <= 0)
			return Error::IO_ERROR;
		data += n;
		size -= static_cast<size_t>(n);
	}
	return Error::OK;
}

Error writeZeros(IFileStream& out, uint64_t count)
{
	static const uint8_t zeros[4096] = {};
	while (count > 0)
	{
		size_t n = static_cast<size_t>(std::min<uint64_t>(count, sizeof(zeros)));
		Error e = writeAll(out, zeros, n);
		if (e != Error::OK)
			return e;
		count -= n;
	}
	return Error::OK;
}

// 读入整个本机文件
Error readNativeFile(const std::string& path, std::vector<uint8_t>& out)
{
	NativeFileStream fs;
	if (!fs.open(path, IFileStream::Mode::READ))
		return native::errorFromErrno(errno);
	int64_t size = fs.size();
	if (size < 0)
		return Error::IO_ERROR;
	out.resize(static_cast<size_t>(size));
	size_t got = 0;
	while (got < out.size())
	{
		int n = fs.read(out.data() + got, static_cast<unsigned int>(std::min<size_t>(out.size() - got, VFS_MAX_IO_CHUNK)));
		if (n < 0)
			return Error::IO_ERROR;
		if (n == 0)
			break;
		got += static_cast<size_t>(n);
	}
	out.resize(got);
	return Error::OK;
}

// ASCII 小写折叠，用于检查大小写冲突
std::string foldCase(std::string_view s)
{
	std::string r(s);
	for (auto& c : r)
	{
		if (c >= 'A' && c <= 'Z')
			c = static_cast<char>(c - 'A' + 'a');
	}
	return r;
}

} // namespace

ArchiveWriter::ArchiveWriter(const ArchiveWriteOptions& options)
	: m_options(options)
{
}

ArchiveWriter::~ArchiveWriter()
{
}

Error ArchiveWriter::fail(Error err, std::string message)
{
	m_lastError = std::move(message);
	return err;
}

Error ArchiveWriter::addFile(std::string_view archivePath, std::string_view nativePath, const FileWriteOptions& options)
{
	Pending p;
	Error e = Path::normalizeRelative(archivePath, p.path);
	if (e != Error::OK || p.path.empty() || p.path.size() > MAX_PATH_LENGTH)
		return fail(Error::INVALID_PATH, "非法的归档路径: " + std::string(archivePath));
	p.type = EntryType::FILE;
	p.nativePath = std::string(nativePath);
	p.options = options;
	m_pending.push_back(std::move(p));
	return Error::OK;
}

Error ArchiveWriter::addData(std::string_view archivePath, std::vector<uint8_t> data, const FileWriteOptions& options)
{
	Pending p;
	Error e = Path::normalizeRelative(archivePath, p.path);
	if (e != Error::OK || p.path.empty() || p.path.size() > MAX_PATH_LENGTH)
		return fail(Error::INVALID_PATH, "非法的归档路径: " + std::string(archivePath));
	p.type = EntryType::FILE;
	p.data = std::make_shared<std::vector<uint8_t>>(std::move(data));
	p.options = options;
	m_pending.push_back(std::move(p));
	return Error::OK;
}

Error ArchiveWriter::addDir(std::string_view archivePath)
{
	Pending p;
	Error e = Path::normalizeRelative(archivePath, p.path);
	if (e != Error::OK || p.path.size() > MAX_PATH_LENGTH)
		return fail(Error::INVALID_PATH, "非法的归档路径: " + std::string(archivePath));
	// 根目录不需要条目
	if (p.path.empty())
		return Error::OK;
	p.type = EntryType::DIR;
	m_pending.push_back(std::move(p));
	return Error::OK;
}

Error ArchiveWriter::prepare(std::vector<Pending>& sorted)
{
	sorted = m_pending;
	// 稳定排序：按 path 字节序（可复现的关键）
	std::stable_sort(sorted.begin(), sorted.end(), [](const Pending& a, const Pending& b) { return a.path < b.path; });

	// 重复路径：同名目录合并，其他情况报错
	std::vector<Pending> unique;
	unique.reserve(sorted.size());
	for (auto& p : sorted)
	{
		if (!unique.empty() && unique.back().path == p.path)
		{
			if (unique.back().type == EntryType::DIR && p.type == EntryType::DIR)
				continue;
			return fail(Error::EXISTS, "重复的归档路径: " + p.path);
		}
		unique.push_back(std::move(p));
	}

	// 大小写冲突：在大小写不敏感的平台上会互相覆盖
	std::unordered_map<std::string, std::string> folded;
	folded.reserve(unique.size());
	for (const auto& p : unique)
	{
		auto res = folded.emplace(foldCase(p.path), p.path);
		if (!res.second)
			return fail(Error::EXISTS, "大小写冲突: " + res.first->second + " <-> " + p.path);
	}

	// 文件/目录冲突，以及去掉非空目录：排好序后，"a" 的子孙一定紧跟在 "a/..." 的位置附近，
	// 用“下一个以 path + '/' 开头的条目”来判断
	std::vector<Pending> result;
	result.reserve(unique.size());
	for (size_t i = 0; i < unique.size(); ++i)
	{
		const Pending& p = unique[i];
		std::string prefix = p.path + "/";
		// 在 unique 中二分查找第一个 >= prefix 的条目
		auto it = std::lower_bound(unique.begin(), unique.end(), prefix,
								   [](const Pending& a, const std::string& v) { return a.path < v; });
		bool hasChildren = it != unique.end() && it->path.compare(0, prefix.size(), prefix) == 0;
		if (p.type == EntryType::FILE && hasChildren)
			return fail(Error::NOT_DIR, "路径既是文件又是目录: " + p.path);
		if (p.type == EntryType::DIR && hasChildren)
			continue;   // 非空目录由文件路径推导，不写条目
		result.push_back(p);
	}
	sorted = std::move(result);
	return Error::OK;
}

Error ArchiveWriter::write(std::string_view outPath, ArchiveWriteStats* stats)
{
	std::string out(outPath);
	std::string tmp = out + ".tmp";
	NativeFileStream fs;
	if (!fs.open(tmp, IFileStream::Mode::WRITE))
		return fail(native::errorFromErrno(errno), "无法创建输出文件: " + tmp);

	Error e = write(fs, stats);
	fs.close();
	std::error_code ec;
	if (e != Error::OK)
	{
		std::filesystem::remove(native::toFsPath(tmp), ec);
		return e;
	}
	// 写完再 rename，中途崩溃不会留下半个归档
	std::filesystem::rename(native::toFsPath(tmp), native::toFsPath(out), ec);
	if (ec)
	{
		std::filesystem::remove(native::toFsPath(tmp), ec);
		return fail(native::errorFromCode(ec), "无法重命名到: " + out);
	}
	return Error::OK;
}

Error ArchiveWriter::write(IFileStream& out, ArchiveWriteStats* stats)
{
	m_lastError.clear();
	const uint32_t alignment = m_options.alignment;
	if (alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment > MAX_ALIGNMENT)
		return fail(Error::UNSUPPORTED, "alignment 必须是 1..65536 之间的 2 的幂");
	if (m_options.fileKeyLength == 0 || m_options.fileKeyLength > MAX_FILE_KEY_SIZE)
		return fail(Error::UNSUPPORTED, "文件 XOR key 长度必须在 1..32 之间");
	if (!m_options.indexKey.empty() && m_options.indexKey.size() != INDEX_KEY_SIZE)
		return fail(Error::KEY_INVALID, "索引密钥必须是 32 字节");

	std::vector<Pending> items;
	Error e = prepare(items);
	if (e != Error::OK)
		return e;
	if (items.size() > UINT32_MAX)
		return fail(Error::UNSUPPORTED, "条目过多");

	ArchiveWriteStats st;
	std::vector<ArchiveEntry> entries(items.size());

	// 1. 占位 header + 对齐到数据区起点
	if (out.seek(0, SEEK_SET) != 0)
		return fail(Error::IO_ERROR, "输出流不支持 seek");
	const uint64_t dataOffset = alignUp(HEADER_SIZE, alignment);
	e = writeZeros(out, dataOffset);
	if (e != Error::OK)
		return fail(e, "写入失败");
	uint64_t pos = dataOffset;

	std::map<DedupKey, DedupValue> dedup;
	std::vector<uint8_t> content;
	std::vector<uint8_t> compressed;
	std::vector<uint8_t> buffer(IO_BUFFER_SIZE);

	// 2. 数据区：按 path 顺序逐个写入
	for (size_t i = 0; i < items.size(); ++i)
	{
		const Pending& item = items[i];
		ArchiveEntry& entry = entries[i];
		entry.path = item.path;
		entry.type = item.type;
		if (item.type == EntryType::DIR)
		{
			++st.dirCount;
			continue;
		}
		++st.fileCount;
		entry.mtime = item.options.mtime;

		// 2.1 取得内容与哈希。压缩或内存数据：整体放进内存；否则流式计算哈希（第一遍）
		const bool inMemory = item.data != nullptr || item.options.compress;
		Hash128 contentHash;
		uint64_t size = 0;
		std::unique_ptr<NativeFileStream> file;
		const std::vector<uint8_t>* data = nullptr;
		if (inMemory)
		{
			if (item.data)
				data = item.data.get();
			else
			{
				e = readNativeFile(item.nativePath, content);
				if (e != Error::OK)
					return fail(e, "无法读取文件: " + item.nativePath);
				data = &content;
			}
			size = data->size();
			contentHash = xxh3_128(data->data(), data->size());
		}
		else
		{
			file = std::make_unique<NativeFileStream>();
			if (!file->open(item.nativePath, IFileStream::Mode::READ))
				return fail(native::errorFromErrno(errno), "无法读取文件: " + item.nativePath);
			Xxh3Stream hasher;
			while (true)
			{
				int n = file->read(buffer.data(), static_cast<unsigned int>(buffer.size()));
				if (n < 0)
					return fail(Error::IO_ERROR, "读取失败: " + item.nativePath);
				if (n == 0)
					break;
				hasher.update(buffer.data(), static_cast<size_t>(n));
				size += static_cast<uint64_t>(n);
			}
			contentHash = hasher.digest();
		}
		entry.size = size;
		entry.hash = contentHash.low;
		st.totalSize += size;

		// 2.2 存储方式：先压缩，再加密
		const uint8_t* storedData = nullptr;
		entry.compression = CompressionType::NONE;
		if (item.options.compress && size > 0 && size <= Compression::lz4MaxInputSize())
		{
			if (Compression::lz4Compress(data->data(), data->size(), compressed, m_options.compressionLevel) &&
				compressed.size() < data->size())
			{
				entry.compression = CompressionType::LZ4;
				storedData = compressed.data();
				entry.storedSize = compressed.size();
				++st.compressedCount;
			}
		}
		if (entry.compression == CompressionType::NONE)
		{
			entry.storedSize = size;
			if (inMemory)
				storedData = data->data();
		}
		if (item.options.encrypt && size > 0)
		{
			entry.cipher = CipherType::XOR;
			entry.keyLength = static_cast<uint8_t>(m_options.fileKeyLength);
			deriveFileKey(contentHash, entry.key.data(), m_options.fileKeyLength);
			++st.encryptedCount;
		}

		// 2.3 去重：同样内容、同样存储方式的数据只写一份
		DedupKey key{contentHash, size, static_cast<uint8_t>(entry.cipher), static_cast<uint8_t>(entry.compression),
					 entry.keyLength};
		if (m_options.dedup && size > 0)
		{
			auto it = dedup.find(key);
			if (it != dedup.end())
			{
				entry.offset = it->second.offset;
				entry.storedSize = it->second.storedSize;
				++st.dedupCount;
				continue;
			}
		}

		// 2.4 对齐并写入。空文件不占数据，也不需要对齐
		if (entry.storedSize > 0)
		{
			uint64_t aligned = alignUp(pos, alignment);
			e = writeZeros(out, aligned - pos);
			if (e != Error::OK)
				return fail(e, "写入失败");
			pos = aligned;
		}
		entry.offset = pos;

		if (entry.storedSize == 0)
		{
			// 空文件：没有数据要写（内存中的空 vector 的 data() 可能是 nullptr）
		}
		else if (storedData)
		{
			// 内存中的数据：按块复制、加密、写入（不修改原数据）
			uint64_t done = 0;
			while (done < entry.storedSize)
			{
				size_t n = static_cast<size_t>(std::min<uint64_t>(entry.storedSize - done, buffer.size()));
				std::memcpy(buffer.data(), storedData + done, n);
				if (entry.cipher == CipherType::XOR)
					xorCipher(buffer.data(), n, entry.key.data(), entry.keyLength, done);
				e = writeAll(out, buffer.data(), n);
				if (e != Error::OK)
					return fail(e, "写入失败");
				done += n;
			}
		}
		else
		{
			// 大文件：第二遍流式读取、加密、写入，内存占用固定 1 MB
			if (file->seek(0, SEEK_SET) != 0)
				return fail(Error::IO_ERROR, "seek 失败: " + item.nativePath);
			uint64_t done = 0;
			while (done < size)
			{
				int n = file->read(buffer.data(), static_cast<unsigned int>(buffer.size()));
				if (n <= 0)
					break;
				size_t len = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(n), size - done));
				if (entry.cipher == CipherType::XOR)
					xorCipher(buffer.data(), len, entry.key.data(), entry.keyLength, done);
				e = writeAll(out, buffer.data(), len);
				if (e != Error::OK)
					return fail(e, "写入失败");
				done += len;
			}
			// 两遍之间文件被修改：拒绝生成内容与哈希不一致的归档
			if (done != size)
				return fail(Error::IO_ERROR, "打包过程中文件被修改: " + item.nativePath);
		}
		pos += entry.storedSize;
		st.storedSize += entry.storedSize;
		if (m_options.dedup && size > 0)
			dedup.emplace(key, DedupValue{entry.offset, entry.storedSize});
	}
	const uint64_t dataSize = pos - dataOffset;

	// 3. 构建明文索引：Entry[n] + StringPool
	const size_t entryCount = entries.size();
	size_t poolSize = 0;
	for (const auto& entry : entries)
		poolSize += entry.path.size();
	std::vector<uint8_t> index(entryCount * ENTRY_SIZE + poolSize, 0);
	uint8_t* pool = index.data() + entryCount * ENTRY_SIZE;
	uint32_t nameOffset = 0;
	for (size_t i = 0; i < entryCount; ++i)
	{
		const ArchiveEntry& entry = entries[i];
		uint8_t* p = index.data() + i * ENTRY_SIZE;
		p[E_TYPE] = static_cast<uint8_t>(entry.type);                        // [0]  type
		p[E_CIPHER] = static_cast<uint8_t>(entry.cipher);                    // [1]  cipher
		p[E_COMPRESSION] = static_cast<uint8_t>(entry.compression);          // [2]  compression
		p[E_KEY_LENGTH] = entry.keyLength;                                   // [3]  keyLength
		ByteIO::writeU32LE(p + E_NAME_OFFSET, nameOffset);                   // [4]  nameOffset
		ByteIO::writeU32LE(p + E_NAME_LENGTH, static_cast<uint32_t>(entry.path.size())); // [8] nameLength
		ByteIO::writeU64LE(p + E_OFFSET, entry.offset);                      // [16] offset
		ByteIO::writeU64LE(p + E_STORED_SIZE, entry.storedSize);             // [24] storedSize
		ByteIO::writeU64LE(p + E_SIZE, entry.size);                          // [32] size
		ByteIO::writeU64LE(p + E_MTIME, static_cast<uint64_t>(entry.mtime)); // [40] mtime
		ByteIO::writeU64LE(p + E_HASH, entry.hash);                          // [48] hash
		std::memcpy(p + E_KEY, entry.key.data(), entry.keyLength);           // [64] key
		std::memcpy(pool + nameOffset, entry.path.data(), entry.path.size());
		nameOffset += static_cast<uint32_t>(entry.path.size());
	}

	// 4. 索引加密：密钥与 nonce 都由明文索引确定性派生（可复现），也可以使用外部密钥
	ArchiveHeader h;
	h.alignment = alignment;
	h.entryCount = static_cast<uint32_t>(entryCount);
	h.dataOffset = dataOffset;
	h.dataSize = dataSize;
	h.indexOffset = pos;
	h.indexSize = index.size();
	h.keyCheck = static_cast<uint32_t>(xxh3_64(index.data(), index.size()));
	if (m_options.encryptIndex)
	{
		h.flags |= ArchiveFlags::IndexEncrypted;
		std::array<uint8_t, INDEX_KEY_SIZE> key{};
		if (!m_options.indexKey.empty())
		{
			std::memcpy(key.data(), m_options.indexKey.data(), INDEX_KEY_SIZE);
			if (m_options.storeKey)
				h.flags |= ArchiveFlags::KeyStored;
		}
		else
		{
			// 没有外部密钥：派生密钥并存入文件，只起混淆作用
			for (uint32_t i = 0; i < INDEX_KEY_SIZE; i += 16)
			{
				Hash128 k = xxh3_128(index.data(), index.size(), SEED_INDEX_KEY + i);
				ByteIO::writeU64LE(key.data() + i, k.low);
				ByteIO::writeU64LE(key.data() + i + 8, k.high);
			}
			h.flags |= ArchiveFlags::KeyStored;
		}
		Hash128 n = xxh3_128(index.data(), index.size(), SEED_INDEX_NONCE);
		uint8_t nonceBytes[16];
		ByteIO::writeU64LE(nonceBytes, n.low);
		ByteIO::writeU64LE(nonceBytes + 8, n.high);
		std::memcpy(h.indexNonce.data(), nonceBytes, INDEX_NONCE_SIZE);
		if (h.flags & ArchiveFlags::KeyStored)
			h.indexKey = key;

		ChaCha20 cipher(key.data(), h.indexNonce.data(), INDEX_CHACHA_COUNTER);
		cipher.process(index.data(), index.size());
	}
	h.indexHash = xxh3_64(index.data(), index.size());

	e = writeAll(out, index.data(), index.size());
	if (e != Error::OK)
		return fail(e, "写入索引失败");
	pos += index.size();

	// 5. footer：用于定位附加在其他文件后面的归档
	uint8_t footer[FOOTER_SIZE];
	std::memcpy(footer + F_MAGIC, FOOTER_MAGIC, sizeof(FOOTER_MAGIC));     // [0] magic
	ByteIO::writeU64LE(footer + F_ARCHIVE_SIZE, pos + FOOTER_SIZE);       // [8] archiveSize
	e = writeAll(out, footer, FOOTER_SIZE);
	if (e != Error::OK)
		return fail(e, "写入 footer 失败");
	pos += FOOTER_SIZE;

	// 6. 回填 header
	uint8_t headerBuf[HEADER_SIZE];
	h.serialize(headerBuf);
	if (out.seek(0, SEEK_SET) != 0)
		return fail(Error::IO_ERROR, "输出流不支持 seek");
	e = writeAll(out, headerBuf, HEADER_SIZE);
	if (e != Error::OK)
		return fail(e, "写入 header 失败");
	// 输出流原有内容可能更长（例如复用旧文件），截断到归档长度
	if (out.size() > static_cast<int64_t>(pos))
		out.resize(static_cast<int64_t>(pos));

	st.archiveSize = pos;
	if (stats)
		*stats = st;
	return Error::OK;
}

NS_VFS_END
