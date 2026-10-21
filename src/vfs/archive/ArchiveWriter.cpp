#include "ArchiveWriter.h"
#include <atomic>
#include <new>
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

// Seed constants used for derivation. Changing them changes the output bytes; they are part of the format behavior, so do not change them casually
constexpr uint64_t SEED_FILE_KEY = 0x5646532D4B455931ull;    // "VFS-KEY1"
constexpr uint64_t SEED_INDEX_KEY = 0x5646532D49445831ull;   // "VFS-IDX1"
constexpr uint64_t SEED_INDEX_NONCE = 0x5646532D4E4F4E31ull; // "VFS-NON1"

constexpr size_t IO_BUFFER_SIZE = 1u << 20;   // 1 MB streaming buffer

// Dedup key: content hash + length + storage method. The XOR key is derived from the content hash, so identical content gets the same key
struct DedupKey
{
	Hash128 hash;
	uint64_t size;
	uint8_t cipher;
	uint8_t compression;
	uint8_t keyLength;
	uint8_t blockLog2;

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
		if (keyLength != o.keyLength)
			return keyLength < o.keyLength;
		return blockLog2 < o.blockLog2;
	}
};

struct DedupValue
{
	uint64_t offset;
	uint64_t storedSize;
};

// Deterministically derive a keyLen-byte XOR key from the content hash (not random, so the result is reproducible)
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

// Block-compress data: out = block table (u64 LE end offset per block, bit 63 = raw) + blocks.
// A block that does not shrink is stored raw. Output depends only on the input, so packing stays reproducible.
bool compressBlocks(const uint8_t* data, uint64_t size, uint8_t log2, int level, std::vector<uint8_t>& out,
					std::vector<uint8_t>& scratch)
{
	const uint64_t blockSize = uint64_t(1) << log2;
	const uint64_t blocks = (size + blockSize - 1) / blockSize;
	const uint64_t table = blocks * BLOCK_ENTRY_SIZE;
	out.assign(static_cast<size_t>(table), 0);
	uint64_t end = 0;
	for (uint64_t b = 0; b < blocks; ++b)
	{
		const uint8_t* src = data + b * blockSize;
		const size_t len = static_cast<size_t>(std::min<uint64_t>(blockSize, size - b * blockSize));
		uint64_t item = 0;
		if (Compression::lz4Compress(src, len, scratch, level) && scratch.size() < len)
		{
			out.insert(out.end(), scratch.begin(), scratch.end());
			end += scratch.size();
			item = end;
		}
		else
		{
			out.insert(out.end(), src, src + len);
			end += len;
			item = end | BLOCK_RAW_FLAG;
		}
		ByteIO::writeU64LE(out.data() + b * BLOCK_ENTRY_SIZE, item);
		// Stop early once the result can no longer be smaller than the original
		if (out.size() >= size)
			return false;
	}
	return true;
}

uint64_t alignUp(uint64_t v, uint32_t alignment)
{
	return (v + alignment - 1) / alignment * alignment;
}

// Write exactly size bytes
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

// Read an entire native file
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

// ASCII lower-case fold, used to detect case conflicts
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
		return fail(Error::INVALID_PATH, "invalid archive path: " + std::string(archivePath));
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
		return fail(Error::INVALID_PATH, "invalid archive path: " + std::string(archivePath));
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
		return fail(Error::INVALID_PATH, "invalid archive path: " + std::string(archivePath));
	// The root directory does not need an entry
	if (p.path.empty())
		return Error::OK;
	p.type = EntryType::DIR;
	m_pending.push_back(std::move(p));
	return Error::OK;
}

Error ArchiveWriter::prepare(std::vector<Pending>& sorted)
{
	sorted = m_pending;
	// Stable sort: by path byte order (essential for reproducibility)
	std::stable_sort(sorted.begin(), sorted.end(), [](const Pending& a, const Pending& b) { return a.path < b.path; });

	// Duplicate paths: directories with the same name are merged; anything else is an error
	std::vector<Pending> unique;
	unique.reserve(sorted.size());
	for (auto& p : sorted)
	{
		if (!unique.empty() && unique.back().path == p.path)
		{
			if (unique.back().type == EntryType::DIR && p.type == EntryType::DIR)
				continue;
			return fail(Error::EXISTS, "duplicate archive path: " + p.path);
		}
		unique.push_back(std::move(p));
	}

	// Case conflict: on a case-insensitive platform these would overwrite each other
	std::unordered_map<std::string, std::string> folded;
	folded.reserve(unique.size());
	for (const auto& p : unique)
	{
		auto res = folded.emplace(foldCase(p.path), p.path);
		if (!res.second)
			return fail(Error::EXISTS, "case conflict: " + res.first->second + " <-> " + p.path);
	}

	// File/directory conflicts, and dropping non-empty directories: after sorting, descendants of "a" sit next to the "a/..." entries,
	// so the test is "the next entry that starts with path + '/'"
	std::vector<Pending> result;
	result.reserve(unique.size());
	for (size_t i = 0; i < unique.size(); ++i)
	{
		const Pending& p = unique[i];
		std::string prefix = p.path + "/";
		// Binary-search unique for the first entry >= prefix
		auto it = std::lower_bound(unique.begin(), unique.end(), prefix,
								   [](const Pending& a, const std::string& v) { return a.path < v; });
		bool hasChildren = it != unique.end() && it->path.compare(0, prefix.size(), prefix) == 0;
		if (p.type == EntryType::FILE && hasChildren)
			return fail(Error::NOT_DIR, "path is both a file and a directory: " + p.path);
		if (p.type == EntryType::DIR && hasChildren)
			continue;   // A non-empty directory is implied by file paths and is not written as an entry
		result.push_back(p);
	}
	sorted = std::move(result);
	return Error::OK;
}

Error ArchiveWriter::write(std::string_view outPath, ArchiveWriteStats* stats)
{
	std::string out(outPath);
	// The temporary name includes the process id and an in-process counter: two processes (or threads) packing to the same path will not truncate or delete each other's temporary file
	static std::atomic<uint64_t> s_counter{0};
	std::string tmp = out + "." + std::to_string(native::processId()) + "-" +
					  std::to_string(s_counter.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
	NativeFileStream fs;
	if (!fs.open(tmp, IFileStream::Mode::WRITE))
		return fail(native::errorFromErrno(errno), "cannot create output file: " + tmp);

	Error e = Error::OK;
	try
	{
		e = write(fs, stats);
	}
	catch (const std::bad_alloc&)
	{
		// Out of memory while packing: remove the temporary file below instead of leaking it
		e = Error::IO_ERROR;
	}
	fs.close();
	if (e != Error::OK)
	{
		native::removeFilePath(tmp);
		return e;
	}
	// Rename only after the write finishes, so a crash midway does not leave a half-written archive. On Windows the old archive can still be replaced even while a game has it open (with FILE_SHARE_DELETE)
	e = native::renamePath(tmp, out);
	if (e != Error::OK)
	{
		native::removeFilePath(tmp);
		return fail(e, "cannot rename to: " + out);
	}
	return Error::OK;
}

Error ArchiveWriter::write(IFileStream& out, ArchiveWriteStats* stats)
{
	m_lastError.clear();
	const uint32_t alignment = m_options.alignment;
	if (alignment == 0 || (alignment & (alignment - 1)) != 0 || alignment > MAX_ALIGNMENT)
		return fail(Error::UNSUPPORTED, "alignment must be a power of two in 1..65536");
	if (m_options.blockLog2 < MIN_BLOCK_LOG2 || m_options.blockLog2 > MAX_BLOCK_LOG2)
		return fail(Error::UNSUPPORTED, "LZ4 block size must be 4 KiB..4 MiB (log2 12..22)");
	if (m_options.fileKeyLength == 0 || m_options.fileKeyLength > MAX_FILE_KEY_SIZE)
		return fail(Error::UNSUPPORTED, "file XOR key length must be in 1..32");
	if (!m_options.indexKey.empty() && m_options.indexKey.size() != INDEX_KEY_SIZE)
		return fail(Error::KEY_INVALID, "index key must be 32 bytes");

	std::vector<Pending> items;
	Error e = prepare(items);
	if (e != Error::OK)
		return e;
	if (items.size() > UINT32_MAX)
		return fail(Error::UNSUPPORTED, "too many entries");

	ArchiveWriteStats st;
	std::vector<ArchiveEntry> entries(items.size());

	// 1. Placeholder header + align to the start of the data region
	if (out.seek(0, SEEK_SET) != 0)
		return fail(Error::IO_ERROR, "output stream does not support seek");
	const uint64_t dataOffset = alignUp(HEADER_SIZE, alignment);
	e = writeZeros(out, dataOffset);
	if (e != Error::OK)
		return fail(e, "write failed");
	uint64_t pos = dataOffset;

	std::map<DedupKey, DedupValue> dedup;
	std::vector<uint8_t> content;
	std::vector<uint8_t> compressed;
	std::vector<uint8_t> block;
	std::vector<uint8_t> buffer(IO_BUFFER_SIZE);

	// 2. Data region: write each item in path order
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

		// 2.1 Obtain the content and its hash. Compressed or in-memory data is held entirely in memory; otherwise hash it as a stream (first pass)
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
					return fail(e, "cannot read file: " + item.nativePath);
				data = &content;
			}
			size = data->size();
			contentHash = xxh3_128(data->data(), data->size());
		}
		else
		{
			file = std::make_unique<NativeFileStream>();
			if (!file->open(item.nativePath, IFileStream::Mode::READ))
				return fail(native::errorFromErrno(errno), "cannot read file: " + item.nativePath);
			Xxh3Stream hasher;
			while (true)
			{
				int n = file->read(buffer.data(), static_cast<unsigned int>(buffer.size()));
				if (n < 0)
					return fail(Error::IO_ERROR, "read failed: " + item.nativePath);
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

		// 2.2 Storage method: compress first, then encrypt
		const uint8_t* storedData = nullptr;
		entry.compression = CompressionType::NONE;
		if (item.options.compress && size > 0)
		{
			const uint8_t log2 = item.options.blockLog2 ? item.options.blockLog2 : m_options.blockLog2;
			if (log2 < MIN_BLOCK_LOG2 || log2 > MAX_BLOCK_LOG2)
				return fail(Error::UNSUPPORTED, "LZ4 block size must be 4 KiB..4 MiB (log2 12..22) for: " + item.path);
			if (compressBlocks(data->data(), size, log2, m_options.compressionLevel, compressed, block) &&
				compressed.size() < size)
			{
				entry.compression = CompressionType::LZ4;
				entry.blockLog2 = log2;
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

		// 2.3 Dedup: data with the same content and the same storage method is written only once
		DedupKey key{contentHash, size, static_cast<uint8_t>(entry.cipher), static_cast<uint8_t>(entry.compression),
					 entry.keyLength, entry.blockLog2};
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

		// 2.4 Align and write. An empty file occupies no data and needs no alignment
		if (entry.storedSize > 0)
		{
			uint64_t aligned = alignUp(pos, alignment);
			e = writeZeros(out, aligned - pos);
			if (e != Error::OK)
				return fail(e, "write failed");
			pos = aligned;
		}
		entry.offset = pos;

		if (entry.storedSize == 0)
		{
			// Empty file: nothing to write (data() of an empty in-memory vector may be nullptr)
		}
		else if (storedData)
		{
			// In-memory data: copy, encrypt, and write in chunks (the original data is not modified)
			uint64_t done = 0;
			while (done < entry.storedSize)
			{
				size_t n = static_cast<size_t>(std::min<uint64_t>(entry.storedSize - done, buffer.size()));
				std::memcpy(buffer.data(), storedData + done, n);
				if (entry.cipher == CipherType::XOR)
					xorCipher(buffer.data(), n, entry.key.data(), entry.keyLength, done);
				e = writeAll(out, buffer.data(), n);
				if (e != Error::OK)
					return fail(e, "write failed");
				done += n;
			}
		}
		else
		{
			// Large file: second pass reads, encrypts, and writes as a stream, with a fixed 1 MB memory footprint
			if (file->seek(0, SEEK_SET) != 0)
				return fail(Error::IO_ERROR, "seek failed: " + item.nativePath);
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
					return fail(e, "write failed");
				done += len;
			}
			// The file changed between the two passes: refuse to produce an archive whose content does not match its hash
			if (done != size)
				return fail(Error::IO_ERROR, "file modified while packing: " + item.nativePath);
		}
		pos += entry.storedSize;
		st.storedSize += entry.storedSize;
		if (m_options.dedup && size > 0)
			dedup.emplace(key, DedupValue{entry.offset, entry.storedSize});
	}
	const uint64_t dataSize = pos - dataOffset;

	// 3. Build the plaintext index: Entry[n] + StringPool
	const size_t entryCount = entries.size();
	uint64_t poolSize = 0;
	for (const auto& entry : entries)
		poolSize += entry.path.size();
	// Same limit as the reader (VFS_MAX_INDEX_SIZE): otherwise the writer would emit an archive that cannot be read
	if (entryCount > UINT32_MAX || static_cast<uint64_t>(entryCount) * ENTRY_SIZE + poolSize > VFS_MAX_INDEX_SIZE)
		return fail(Error::UNSUPPORTED, "index too large: " + std::to_string(entryCount) + " entries");
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
		p[E_BLOCK_LOG2] = entry.blockLog2;                                   // [12] blockLog2
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

	// 4. Index encryption: both the key and the nonce are derived deterministically from the plaintext index (reproducible), or an external key may be used
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
			// No external key: derive a key and store it in the file; this is obfuscation only
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
		return fail(e, "failed to write index");
	pos += index.size();

	// 5. footer: used to locate an archive appended after another file
	uint8_t footer[FOOTER_SIZE];
	std::memcpy(footer + F_MAGIC, FOOTER_MAGIC, sizeof(FOOTER_MAGIC));     // [0] magic
	ByteIO::writeU64LE(footer + F_ARCHIVE_SIZE, pos + FOOTER_SIZE);       // [8] archiveSize
	e = writeAll(out, footer, FOOTER_SIZE);
	if (e != Error::OK)
		return fail(e, "failed to write footer");
	pos += FOOTER_SIZE;

	// 6. Backfill the header
	uint8_t headerBuf[HEADER_SIZE];
	h.serialize(headerBuf);
	if (out.seek(0, SEEK_SET) != 0)
		return fail(Error::IO_ERROR, "output stream does not support seek");
	e = writeAll(out, headerBuf, HEADER_SIZE);
	if (e != Error::OK)
		return fail(e, "failed to write header");
	// The output stream may already be longer (for example when reusing an old file); truncate it to the archive length
	if (out.size() > static_cast<int64_t>(pos))
		out.resize(static_cast<int64_t>(pos));

	st.archiveSize = pos;
	if (stats)
		*stats = st;
	return Error::OK;
}

NS_VFS_END
