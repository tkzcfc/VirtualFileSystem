#include "ArchiveReader.h"
#include <new>
#include <cstdint>
#include "ByteIO.h"
#include "Compression.h"
#include "../Path.h"
#include "../crypto/ChaCha20.h"
#include "../crypto/Hash.h"
#include "../crypto/XorCipher.h"
#include <algorithm>
#include <cstring>

NS_VFS_BEGIN

using namespace ArchiveFormat;

namespace
{

// a + b does not overflow and is <= limit.
inline bool rangeOk(uint64_t a, uint64_t b, uint64_t limit)
{
	return a <= limit && b <= limit - a;
}

inline bool isPowerOfTwo(uint32_t v)
{
	return v != 0 && (v & (v - 1)) == 0;
}

} // namespace

// ---------------------------------------------------------------- ArchiveHeader

void ArchiveHeader::serialize(uint8_t* out)
{
	std::memset(out, 0, HEADER_SIZE);
	std::memcpy(out + H_MAGIC, MAGIC, sizeof(MAGIC));                // [0]   magic
	ByteIO::writeU32LE(out + H_VERSION, version);                     // [8]   version
	ByteIO::writeU32LE(out + H_HEADER_SIZE, headerSize);              // [12]  headerSize
	ByteIO::writeU32LE(out + H_FLAGS, flags);                         // [16]  flags
	ByteIO::writeU32LE(out + H_ALIGNMENT, alignment);                 // [20]  alignment
	ByteIO::writeU32LE(out + H_ENTRY_COUNT, entryCount);              // [24]  entryCount
	ByteIO::writeU32LE(out + H_ENTRY_SIZE, entrySize);                // [28]  entrySize
	ByteIO::writeU64LE(out + H_DATA_OFFSET, dataOffset);              // [32]  dataOffset
	ByteIO::writeU64LE(out + H_DATA_SIZE, dataSize);                  // [40]  dataSize
	ByteIO::writeU64LE(out + H_INDEX_OFFSET, indexOffset);            // [48]  indexOffset
	ByteIO::writeU64LE(out + H_INDEX_SIZE, indexSize);                // [56]  indexSize
	std::memcpy(out + H_INDEX_KEY, indexKey.data(), indexKey.size());       // [64]  indexKey
	std::memcpy(out + H_INDEX_NONCE, indexNonce.data(), indexNonce.size()); // [96]  indexNonce
	ByteIO::writeU32LE(out + H_KEY_CHECK, keyCheck);                  // [108] keyCheck
	ByteIO::writeU64LE(out + H_INDEX_HASH, indexHash);                // [112] indexHash
	headerChecksum = xxh3_64(out, H_CHECKSUM);
	ByteIO::writeU64LE(out + H_CHECKSUM, headerChecksum);             // [120] headerChecksum
}

Error ArchiveHeader::parse(const uint8_t* in)
{
	// Order: magic -> version -> checksum. Version is checked first so a later format change (the header length may differ) reports "version mismatch" rather than "corrupted".
	if (std::memcmp(in + H_MAGIC, MAGIC, sizeof(MAGIC)) != 0)
		return Error::CORRUPTED;
	version = ByteIO::readU32LE(in + H_VERSION);
	if (version != VERSION)
		return Error::VERSION_MISMATCH;
	headerChecksum = ByteIO::readU64LE(in + H_CHECKSUM);
	if (headerChecksum != xxh3_64(in, H_CHECKSUM))
		return Error::CORRUPTED;

	headerSize = ByteIO::readU32LE(in + H_HEADER_SIZE);
	flags = ByteIO::readU32LE(in + H_FLAGS);
	alignment = ByteIO::readU32LE(in + H_ALIGNMENT);
	entryCount = ByteIO::readU32LE(in + H_ENTRY_COUNT);
	entrySize = ByteIO::readU32LE(in + H_ENTRY_SIZE);
	dataOffset = ByteIO::readU64LE(in + H_DATA_OFFSET);
	dataSize = ByteIO::readU64LE(in + H_DATA_SIZE);
	indexOffset = ByteIO::readU64LE(in + H_INDEX_OFFSET);
	indexSize = ByteIO::readU64LE(in + H_INDEX_SIZE);
	std::memcpy(indexKey.data(), in + H_INDEX_KEY, indexKey.size());
	std::memcpy(indexNonce.data(), in + H_INDEX_NONCE, indexNonce.size());
	keyCheck = ByteIO::readU32LE(in + H_KEY_CHECK);
	indexHash = ByteIO::readU64LE(in + H_INDEX_HASH);

	if (headerSize != HEADER_SIZE || entrySize != ENTRY_SIZE)
		return Error::CORRUPTED;
	if (!isPowerOfTwo(alignment) || alignment > MAX_ALIGNMENT)
		return Error::CORRUPTED;
	// Unknown flag bits: this version defines only two.
	if (flags & ~(ArchiveFlags::IndexEncrypted | ArchiveFlags::KeyStored))
		return Error::CORRUPTED;
	return Error::OK;
}

// ---------------------------------------------------------------- ArchiveIndex

const ArchiveEntry* ArchiveIndex::findFile(std::string_view rel) const
{
	auto it = m_files.find(rel);
	if (it == m_files.end())
		return nullptr;
	return &entries[it->second];
}

bool ArchiveIndex::isDir(std::string_view rel) const
{
	return m_dirs.find(rel) != m_dirs.end();
}

const std::vector<DirEntry>* ArchiveIndex::children(std::string_view relDir) const
{
	auto it = m_dirs.find(relDir);
	if (it == m_dirs.end())
		return nullptr;
	return &it->second;
}

Error ArchiveIndex::stat(std::string_view rel, Stat& out) const
{
	if (const ArchiveEntry* e = findFile(rel))
	{
		out.type = FileType::FILE;
		out.size = static_cast<int64_t>(e->size);
		// When an entry's mtime is 0, fall back to the archive file's own mtime.
		out.mtime = e->mtime != 0 ? e->mtime : sourceMtime;
		return Error::OK;
	}
	if (isDir(rel))
	{
		out.type = FileType::DIR;
		out.size = 0;
		out.mtime = sourceMtime;
		return Error::OK;
	}
	// Miss: if some ancestor is a file, return NOT_DIR per the overlay rules.
	std::string_view p = rel;
	while (!p.empty())
	{
		p = Path::parentRelative(p);
		if (p.empty())
			break;
		if (findFile(p))
			return Error::NOT_DIR;
		if (isDir(p))
			break;
	}
	return Error::NOT_FOUND;
}

Error ArchiveIndex::build()
{
	m_files.clear();
	m_dirs.clear();
	m_files.reserve(entries.size());
	m_dirs[std::string()];

	// Ensure directory dir exists; when creating it, register it under its parent (each directory is registered once).
	auto ensureDir = [this](std::string_view dir)
	{
		// Walk from deep to shallow to the first ancestor that already exists, then create levels from shallow to deep.
		std::vector<std::string_view> missing;
		std::string_view p = dir;
		while (!p.empty() && m_dirs.find(p) == m_dirs.end())
		{
			missing.push_back(p);
			p = Path::parentRelative(p);
		}
		for (auto it = missing.rbegin(); it != missing.rend(); ++it)
		{
			m_dirs[std::string(*it)];
			m_dirs[std::string(Path::parentRelative(*it))].push_back(
				DirEntry{std::string(Path::fileName(*it)), FileType::DIR});
		}
	};

	for (uint32_t i = 0; i < entries.size(); ++i)
	{
		const ArchiveEntry& e = entries[i];
		if (e.type == EntryType::DIR)
		{
			ensureDir(e.path);
			continue;
		}
		ensureDir(Path::parentRelative(e.path));
		m_files.emplace(std::string_view(e.path), i);
		m_dirs[std::string(Path::parentRelative(e.path))].push_back(
			DirEntry{std::string(Path::fileName(e.path)), FileType::FILE});
	}

	// The same path is both a file and a directory (for example "a" and "a/b"): the index is invalid.
	for (const auto& kv : m_files)
	{
		if (m_dirs.find(kv.first) != m_dirs.end())
			return Error::CORRUPTED;
	}

	for (auto& kv : m_dirs)
	{
		std::sort(kv.second.begin(), kv.second.end(),
				  [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
	}
	return Error::OK;
}

// ---------------------------------------------------------------- ArchiveReader

Error readFully(IFileStream& stream, int64_t offset, uint8_t* buf, size_t size)
{
	if (stream.seek(offset, SEEK_SET) != offset)
		return Error::IO_ERROR;
	while (size > 0)
	{
		unsigned int chunk = static_cast<unsigned int>(std::min<size_t>(size, VFS_MAX_IO_CHUNK));
		int n = stream.read(buf, chunk);
		if (n < 0)
			return Error::IO_ERROR;
		// EOF before the requested bytes: the file is truncated.
		if (n == 0)
			return Error::CORRUPTED;
		buf += n;
		size -= static_cast<size_t>(n);
	}
	return Error::OK;
}

std::shared_ptr<const ArchiveIndex> ArchiveReader::open(std::string_view nativePath, const ArchiveOptions& options,
														Error* err)
{
	return open(std::make_shared<FileArchiveSource>(std::string(nativePath)), options, err);
}

std::shared_ptr<const ArchiveIndex> ArchiveReader::open(std::shared_ptr<IArchiveSource> source,
														const ArchiveOptions& options, Error* err)
{
	auto fail = [err](Error e) -> std::shared_ptr<const ArchiveIndex>
	{
		setError(err, e);
		return nullptr;
	};
	if (!source)
		return fail(Error::NOT_FOUND);

	Error e = Error::OK;
	std::unique_ptr<IFileStream> stream = source->openStream(&e);
	if (!stream)
		return fail(e);
	const int64_t fileSize = stream->size();
	if (fileSize < 0)
		return fail(Error::IO_ERROR);

	// 1. Locate the archive start.
	int64_t base = options.baseOffset;
	bool viaFooter = false;   // Whether the start was derived backwards from the footer.
	if (base < 0)
	{
		uint8_t magic[8];
		if (fileSize >= static_cast<int64_t>(HEADER_SIZE) && readFully(*stream, 0, magic, 8) == Error::OK &&
			std::memcmp(magic, MAGIC, 8) == 0)
		{
			base = 0;
		}
		else if (fileSize >= static_cast<int64_t>(FOOTER_SIZE))
		{
			// Appended after another file (for example an exe): derive the start from archiveSize in the trailing footer.
			uint8_t footer[FOOTER_SIZE];
			e = readFully(*stream, fileSize - FOOTER_SIZE, footer, FOOTER_SIZE);
			if (e != Error::OK)
				return fail(e);
			if (std::memcmp(footer + F_MAGIC, FOOTER_MAGIC, 8) != 0)
				return fail(Error::CORRUPTED);
			uint64_t archiveSize = ByteIO::readU64LE(footer + F_ARCHIVE_SIZE);
			if (archiveSize > static_cast<uint64_t>(fileSize) || archiveSize < HEADER_SIZE + FOOTER_SIZE)
				return fail(Error::CORRUPTED);
			base = fileSize - static_cast<int64_t>(archiveSize);
			viaFooter = true;
		}
		else
			return fail(Error::CORRUPTED);
	}
	if (base > fileSize || fileSize - base < static_cast<int64_t>(HEADER_SIZE + FOOTER_SIZE))
		return fail(Error::CORRUPTED);
	const uint64_t available = static_cast<uint64_t>(fileSize - base);

	// 2. header
	uint8_t headerBuf[HEADER_SIZE];
	e = readFully(*stream, base, headerBuf, HEADER_SIZE);
	if (e != Error::OK)
		return fail(e);
	auto index = std::make_shared<ArchiveIndex>();
	ArchiveHeader& h = index->header;
	e = h.parse(headerBuf);
	if (e != Error::OK)
		return fail(e);

	// Every region must be in order and in bounds: header <= data <= index <= footer <= available length.
	if (h.dataOffset < HEADER_SIZE || !rangeOk(h.dataOffset, h.dataSize, h.indexOffset))
		return fail(Error::CORRUPTED);
	if (!rangeOk(h.indexOffset, h.indexSize, available) ||
		!rangeOk(h.indexOffset + h.indexSize, FOOTER_SIZE, available))
		return fail(Error::CORRUPTED);
	if (h.indexSize > VFS_MAX_INDEX_SIZE || static_cast<uint64_t>(h.entryCount) * ENTRY_SIZE > h.indexSize)
		return fail(Error::CORRUPTED);
	if (viaFooter)
	{
		// When the start was derived from the footer, the footer must sit immediately after the index and archiveSize must match the header,
		// otherwise trailing garbage after an exe could be treated as an archive. When the start is 0 or given explicitly, format.md section 6 does not depend on the footer.
		uint8_t footer[FOOTER_SIZE];
		e = readFully(*stream, base + static_cast<int64_t>(h.indexOffset + h.indexSize), footer, FOOTER_SIZE);
		if (e != Error::OK)
			return fail(e);
		if (std::memcmp(footer + F_MAGIC, FOOTER_MAGIC, 8) != 0 ||
			ByteIO::readU64LE(footer + F_ARCHIVE_SIZE) != h.indexOffset + h.indexSize + FOOTER_SIZE)
			return fail(Error::CORRUPTED);
	}

	// 3. Read the index and do a fast corruption check (XXH3-64 covers the stored index bytes and does not need the key).
	std::vector<uint8_t> indexBuf(static_cast<size_t>(h.indexSize));
	e = readFully(*stream, base + static_cast<int64_t>(h.indexOffset), indexBuf.data(), indexBuf.size());
	if (e != Error::OK)
		return fail(e);
	if (xxh3_64(indexBuf.data(), indexBuf.size()) != h.indexHash)
		return fail(Error::CORRUPTED);

	// 4. Decrypt the index.
	if (h.flags & ArchiveFlags::IndexEncrypted)
	{
		const uint8_t* key = nullptr;
		if (!options.indexKey.empty())
		{
			if (options.indexKey.size() != INDEX_KEY_SIZE)
				return fail(Error::KEY_INVALID);
			key = options.indexKey.data();
		}
		else if (h.flags & ArchiveFlags::KeyStored)
			key = h.indexKey.data();
		else
			return fail(Error::KEY_REQUIRED);

		ChaCha20 cipher(key, h.indexNonce.data(), INDEX_CHACHA_COUNTER);
		cipher.process(indexBuf.data(), indexBuf.size());
		// Use the low 32 bits of the plaintext hash to tell whether the key is correct, so garbage from a wrong key is not reported as "corrupted".
		if (static_cast<uint32_t>(xxh3_64(indexBuf.data(), indexBuf.size())) != h.keyCheck)
			return fail(Error::KEY_INVALID);
	}

	// 5. Parse entries. Treat every field as untrusted input.
	const uint8_t* pool = indexBuf.data() + static_cast<size_t>(h.entryCount) * ENTRY_SIZE;
	const uint64_t poolSize = h.indexSize - static_cast<uint64_t>(h.entryCount) * ENTRY_SIZE;
	const uint64_t dataEnd = h.dataOffset + h.dataSize;
	index->entries.resize(h.entryCount);
	for (uint32_t i = 0; i < h.entryCount; ++i)
	{
		const uint8_t* p = indexBuf.data() + static_cast<size_t>(i) * ENTRY_SIZE;
		ArchiveEntry& entry = index->entries[i];
		uint8_t type = p[E_TYPE];
		uint8_t cipher = p[E_CIPHER];
		uint8_t compression = p[E_COMPRESSION];
		entry.keyLength = p[E_KEY_LENGTH];
		entry.blockLog2 = p[E_BLOCK_LOG2];
		uint32_t nameOffset = ByteIO::readU32LE(p + E_NAME_OFFSET);
		uint32_t nameLength = ByteIO::readU32LE(p + E_NAME_LENGTH);
		entry.offset = ByteIO::readU64LE(p + E_OFFSET);
		entry.storedSize = ByteIO::readU64LE(p + E_STORED_SIZE);
		entry.size = ByteIO::readU64LE(p + E_SIZE);
		entry.mtime = static_cast<int64_t>(ByteIO::readU64LE(p + E_MTIME));
		entry.hash = ByteIO::readU64LE(p + E_HASH);
		std::memcpy(entry.key.data(), p + E_KEY, entry.key.size());
		// Reserved fields must be 0 (format.md section 4.1): room for a later version, and a tampered index is rejected.
		if (p[E_RESERVED0] != 0 || p[E_RESERVED0 + 1] != 0 || p[E_RESERVED0 + 2] != 0 || ByteIO::readU64LE(p + E_RESERVED1) != 0)
			return fail(Error::CORRUPTED);

		if (type != static_cast<uint8_t>(EntryType::FILE) && type != static_cast<uint8_t>(EntryType::DIR))
			return fail(Error::CORRUPTED);
		if (cipher > static_cast<uint8_t>(CipherType::XOR) || compression > static_cast<uint8_t>(CompressionType::LZ4))
			return fail(Error::CORRUPTED);
		entry.type = static_cast<EntryType>(type);
		entry.cipher = static_cast<CipherType>(cipher);
		entry.compression = static_cast<CompressionType>(compression);

		if (nameLength == 0 || nameLength > MAX_PATH_LENGTH || !rangeOk(nameOffset, nameLength, poolSize))
			return fail(Error::CORRUPTED);
		entry.path.assign(reinterpret_cast<const char*>(pool + nameOffset), nameLength);
		if (!Path::isNormalizedRelative(entry.path))
			return fail(Error::CORRUPTED);
		// Must be strictly ascending: uniqueness, and a reproducible layout.
		if (i > 0 && !(index->entries[i - 1].path < entry.path))
			return fail(Error::CORRUPTED);

		if (entry.type == EntryType::DIR)
		{
			if (entry.offset != 0 || entry.storedSize != 0 || entry.size != 0 || entry.cipher != CipherType::NONE ||
				entry.compression != CompressionType::NONE || entry.keyLength != 0 || entry.blockLog2 != 0)
				return fail(Error::CORRUPTED);
			continue;
		}

		// File data must lie entirely inside the data region. Several entries may point at the same span (content dedup).
		if (entry.offset < h.dataOffset || !rangeOk(entry.offset, entry.storedSize, dataEnd))
			return fail(Error::CORRUPTED);
		if (entry.cipher == CipherType::XOR)
		{
			if (entry.keyLength == 0 || entry.keyLength > MAX_FILE_KEY_SIZE)
				return fail(Error::CORRUPTED);
		}
		else if (entry.keyLength != 0)
			return fail(Error::CORRUPTED);
		if (entry.size > static_cast<uint64_t>(INT64_MAX))
			return fail(Error::CORRUPTED);
		if (entry.compression == CompressionType::NONE && entry.storedSize != entry.size)
			return fail(Error::CORRUPTED);
		if (entry.compression == CompressionType::NONE && entry.blockLog2 != 0)
			return fail(Error::CORRUPTED);
		if (entry.compression == CompressionType::LZ4)
		{
			// Block size in range, and the stored data must at least hold the block table plus one byte per block.
			// Block spans themselves are checked when the table is read.
			if (entry.blockLog2 < MIN_BLOCK_LOG2 || entry.blockLog2 > MAX_BLOCK_LOG2 || entry.size == 0)
				return fail(Error::CORRUPTED);
			const uint64_t blocks = entry.blockCount();
			if (blocks > entry.storedSize / (BLOCK_ENTRY_SIZE + 1) || entry.storedSize - entry.blockTableSize() < blocks)
				return fail(Error::CORRUPTED);
			// LZ4 expands at most about 255:1, so the logical size is bounded by the block payload.
			// This rejects hostile sizes before anyone allocates a buffer for them.
			const uint64_t payload = entry.storedSize - entry.blockTableSize();
			const uint64_t slack = blocks * 64;
			if (entry.size > slack && (entry.size - slack) / 255 > payload)
				return fail(Error::CORRUPTED);
		}
	}

	index->baseOffset = base;
	index->archiveSize = static_cast<int64_t>(h.indexOffset + h.indexSize + FOOTER_SIZE);
	index->sourceMtime = source->mtime();
	index->source = std::move(source);
	e = index->build();
	if (e != Error::OK)
		return fail(e);

	setError(err, Error::OK);
	return index;
}

Error ArchiveReader::readEntry(const ArchiveIndex& index, const ArchiveEntry& entry, std::vector<uint8_t>& out,
							   IFileStream* stream)
{
	if (entry.type != EntryType::FILE)
		return Error::IS_DIR;
	if (entry.storedSize > SIZE_MAX || entry.size > SIZE_MAX)
		return Error::UNSUPPORTED;

	// Entry size was already capped at the LZ4 limit (about 2 GiB) at open, but it can still exceed available memory: a failed allocation returns an error instead of throwing.
	try
	{
		return readEntryImpl(index, entry, out, stream);
	}
	catch (const std::bad_alloc&)
	{
		out.clear();
		return Error::UNSUPPORTED;
	}
}

Error ArchiveReader::readEntryImpl(const ArchiveIndex& index, const ArchiveEntry& entry, std::vector<uint8_t>& out,
								   IFileStream* stream)
{
	std::vector<uint8_t> stored(static_cast<size_t>(entry.storedSize));
	const int64_t offset = index.baseOffset + static_cast<int64_t>(entry.offset);
	std::unique_ptr<IFileStream> owned;
	Error e = Error::OK;
	if (!stream && index.source->supportsReadAt())
	{
		// Positional read: no need to open a new stream.
		size_t got = 0;
		while (got < stored.size())
		{
			int64_t n = index.source->readAt(stored.data() + got, stored.size() - got, static_cast<uint64_t>(offset) + got);
			if (n < 0)
				return Error::IO_ERROR;
			if (n == 0)
				return Error::CORRUPTED;   // Hit the end early: the archive is truncated.
			got += static_cast<size_t>(n);
		}
	}
	else
	{
		if (!stream)
		{
			owned = index.source->openStream(&e);
			if (!owned)
				return e;
			stream = owned.get();
		}
		e = readFully(*stream, offset, stored.data(), stored.size());
		if (e != Error::OK)
			return e;
	}
	// Reverse of packing: decrypt first, then decompress (pack compresses, then encrypts).
	if (entry.cipher == CipherType::XOR)
		xorCipher(stored.data(), stored.size(), entry.key.data(), entry.keyLength, 0);

	if (entry.compression == CompressionType::LZ4)
	{
		out.resize(static_cast<size_t>(entry.size));
		const uint64_t blocks = entry.blockCount();
		const uint64_t table = entry.blockTableSize();
		uint64_t prevEnd = 0;
		for (uint64_t b = 0; b < blocks; ++b)
		{
			uint64_t start = 0, len = 0;
			bool raw = false;
			const uint64_t item = ByteIO::readU64LE(stored.data() + b * BLOCK_ENTRY_SIZE);
			if (!blockSpan(entry, b, prevEnd, item, start, len, raw) ||
				!decodeBlock(entry, b, stored.data() + table + start, static_cast<size_t>(len), raw,
							 out.data() + (b << entry.blockLog2)))
			{
				out.clear();
				return Error::CORRUPTED;
			}
			prevEnd = start + len;
		}
	}
	else
		out = std::move(stored);
	return Error::OK;
}

bool ArchiveReader::blockSpan(const ArchiveEntry& entry, uint64_t block, uint64_t prevEnd, uint64_t item,
							  uint64_t& start, uint64_t& len, bool& raw)
{
	const uint64_t dataSize = entry.storedSize - entry.blockTableSize();
	const uint64_t end = item & BLOCK_END_MASK;
	const uint64_t rawLen = std::min<uint64_t>(entry.blockSize(), entry.size - (block << entry.blockLog2));
	raw = (item & BLOCK_RAW_FLAG) != 0;
	if (end <= prevEnd || end > dataSize)
		return false;
	// The last block must end exactly at the end of the stored data
	if (block + 1 == entry.blockCount() && end != dataSize)
		return false;
	start = prevEnd;
	len = end - prevEnd;
	if (raw)
		return len == rawLen;
	return len <= Compression::lz4CompressBound(static_cast<size_t>(rawLen));
}

bool ArchiveReader::decodeBlock(const ArchiveEntry& entry, uint64_t block, const uint8_t* src, size_t srcLen, bool raw,
								uint8_t* dst)
{
	const size_t rawLen = static_cast<size_t>(std::min<uint64_t>(entry.blockSize(), entry.size - (block << entry.blockLog2)));
	if (raw)
	{
		if (srcLen != rawLen)
			return false;
		std::memcpy(dst, src, rawLen);
		return true;
	}
	return Compression::lz4Decompress(src, srcLen, dst, rawLen);
}

Error ArchiveReader::verify(const ArchiveIndex& index,
							const std::function<void(const ArchiveEntry& entry, Error result)>& cb)
{
	Error e = Error::OK;
	std::unique_ptr<IFileStream> stream = index.source->openStream(&e);
	if (!stream)
		return e;

	Error overall = Error::OK;
	std::vector<uint8_t> data;
	for (const auto& entry : index.entries)
	{
		if (entry.type != EntryType::FILE)
			continue;
		Error result = readEntry(index, entry, data, stream.get());
		if (result == Error::OK && xxh3_128(data.data(), data.size()).low != entry.hash)
			result = Error::CORRUPTED;
		// A hash mismatch or damaged data is reported as CORRUPTED; a read failure (IO_ERROR and similar) keeps the first real error and is not folded into "corrupted".
		if (result != Error::OK && overall == Error::OK)
			overall = result;
		if (cb)
			cb(entry, result);
	}
	return overall;
}

NS_VFS_END
