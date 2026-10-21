// This file: reading a .vfs archive: parse header/footer/index, fast corruption checks, index decryption, and directory-tree construction.
#pragma once

#include "ArchiveFormat.h"
#include "ArchiveSource.h"
#include "../DirEntry.h"
#include <array>
#include <unordered_map>

NS_VFS_BEGIN

/**
 * In-memory form of the header. Field meanings and offsets are in ArchiveFormat.h.
 */
struct ArchiveHeader
{
	uint32_t version = ArchiveFormat::VERSION;
	uint32_t headerSize = ArchiveFormat::HEADER_SIZE;
	uint32_t flags = 0;
	uint32_t alignment = ArchiveFormat::DEFAULT_ALIGNMENT;
	uint32_t entryCount = 0;
	uint32_t entrySize = ArchiveFormat::ENTRY_SIZE;
	uint64_t dataOffset = 0;
	uint64_t dataSize = 0;
	uint64_t indexOffset = 0;
	uint64_t indexSize = 0;
	std::array<uint8_t, ArchiveFormat::INDEX_KEY_SIZE> indexKey{};
	std::array<uint8_t, ArchiveFormat::INDEX_NONCE_SIZE> indexNonce{};
	uint32_t keyCheck = 0;
	uint64_t indexHash = 0;
	uint64_t headerChecksum = 0;

	/**
	 * Serialize into a 128-byte buffer and compute headerChecksum (written back into this struct).
	 */
	void serialize(uint8_t* out);

	/**
	 * Parse a 128-byte buffer. Checks magic, version, checksum, and headerSize/entrySize, in that order.
	 * @return OK / CORRUPTED / VERSION_MISMATCH
	 */
	Error parse(const uint8_t* in);
};

/**
 * An archive entry (one item in the index).
 */
struct ArchiveEntry
{
	std::string path;                                     // Canonical relative path, no leading '/'
	EntryType type = EntryType::FILE;
	CipherType cipher = CipherType::NONE;
	CompressionType compression = CompressionType::NONE;
	uint8_t keyLength = 0;
	uint8_t blockLog2 = 0;                                // LZ4: log2 of the block size; otherwise 0
	uint64_t offset = 0;                                  // Start of stored data (relative to the archive start)
	uint64_t storedSize = 0;                              // Stored size in bytes
	uint64_t size = 0;                                    // Original size in bytes
	int64_t mtime = 0;
	uint64_t hash = 0;                                    // Low 64 bits of XXH3-128(original content)
	std::array<uint8_t, ArchiveFormat::MAX_FILE_KEY_SIZE> key{};

	/// LZ4: block size in bytes.
	uint64_t blockSize() const { return uint64_t(1) << blockLog2; }

	/// LZ4: number of blocks (0 for an empty file).
	uint64_t blockCount() const { return (size >> blockLog2) + ((size & (blockSize() - 1)) != 0 ? 1 : 0); }

	/// LZ4: size of the block table at the start of the stored data.
	uint64_t blockTableSize() const { return blockCount() * ArchiveFormat::BLOCK_ENTRY_SIZE; }
};

/**
 * Read options.
 */
struct ArchiveOptions
{
	std::vector<uint8_t> indexKey;   // When non-empty (must be 32 bytes), overrides the index key stored in the archive
	int64_t baseOffset = -1;         // Archive start; -1 means automatic (magic at offset 0, otherwise the footer at the end of the file)
};

// Transparent hash: look up a std::string key with std::string_view directly, without building a temporary string.
struct TransparentStringHash
{
	using is_transparent = void;

	size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
};

/**
 * Parsed archive index. Immutable after open, shared between the backend and every stream through shared_ptr<const>.
 * Released only after the last stream closes, which implements lazy unmount.
 * Thread safety: read-only, so it may be accessed concurrently without restriction.
 */
class VFS_DLL ArchiveIndex
{
public:
	ArchiveHeader header;                    // Parsed header
	int64_t baseOffset = 0;                  // Offset of the archive start within the source
	int64_t archiveSize = 0;                 // Total archive length (including the footer)
	int64_t sourceMtime = 0;                 // mtime of the source
	std::shared_ptr<IArchiveSource> source;  // Data source (used when opening streams)
	std::vector<ArchiveEntry> entries;       // Ascending byte order of path

	/**
	 * Find a file entry (directories are excluded).
	 * @return nullptr if not found
	 */
	const ArchiveEntry* findFile(std::string_view rel) const;

	/**
	 * Whether this is a directory (an explicit empty directory or a directory implied by a file path; the root is the empty string).
	 */
	bool isDir(std::string_view rel) const;

	/**
	 * Query metadata; returns NOT_DIR when an intermediate component is a file.
	 */
	Error stat(std::string_view rel, Stat& out) const;

	/**
	 * Direct children of a directory (already sorted by name); nullptr if it is not a directory.
	 */
	const std::vector<DirEntry>* children(std::string_view relDir) const;

	/**
	 * Build the lookup table and directory tree from entries.
	 * @return CORRUPTED when a file conflicts with a directory
	 */
	Error build();

private:
	std::unordered_map<std::string_view, uint32_t> m_files;            // path -> index into entries (the view points at strings stored in entries)
	std::unordered_map<std::string, std::vector<DirEntry>, TransparentStringHash, std::equal_to<>> m_dirs; // directory -> children
};

/**
 * Archive reading utilities. All functions are static and thread-safe.
 */
class VFS_DLL ArchiveReader
{
public:

	/**
	 * Open and parse an archive.
	 * Checks: magic, version (VERSION_MISMATCH on mismatch), headerChecksum, every offset/size for out-of-range and overflow,
	 * indexHash (XXH3-64, a fast corruption check), the index key (keyCheck), and the validity of each entry.
	 * @return An immutable index on success; nullptr on failure, with err set to CORRUPTED / VERSION_MISMATCH / KEY_REQUIRED / KEY_INVALID / an IO error
	 */
	static std::shared_ptr<const ArchiveIndex> open(std::shared_ptr<IArchiveSource> source,
													const ArchiveOptions& options = {}, Error* err = nullptr);

	static std::shared_ptr<const ArchiveIndex> open(std::string_view nativePath, const ArchiveOptions& options = {},
													Error* err = nullptr);

	/**
	 * Read the full original content of one file entry (read, XOR decrypt, LZ4 decompress).
	 * @param stream Optional: reuse an already-open source stream; when null, a new one is opened internally
	 */
	static Error readEntry(const ArchiveIndex& index, const ArchiveEntry& entry, std::vector<uint8_t>& out,
						   IFileStream* stream = nullptr);

private:
	static Error readEntryImpl(const ArchiveIndex& index, const ArchiveEntry& entry, std::vector<uint8_t>& out,
							   IFileStream* stream);

public:

	/**
	 * Validate one decoded block-table item of an LZ4 entry and return the block's stored span.
	 * @param entry LZ4 entry
	 * @param block block number
	 * @param prevEnd end offset of the previous block (0 for block 0), relative to the end of the table
	 * @param item  table item (already decrypted)
	 * @param start receives the stored start of the block, relative to the end of the table
	 * @param len   receives the stored length of the block
	 * @param raw   receives whether the block is stored uncompressed
	 * @return false if the item is inconsistent (corrupted archive)
	 */
	static bool blockSpan(const ArchiveEntry& entry, uint64_t block, uint64_t prevEnd, uint64_t item, uint64_t& start,
						  uint64_t& len, bool& raw);

	/**
	 * Decode one stored block into dst (exactly the block's original length).
	 * @return false if the data is corrupt
	 */
	static bool decodeBlock(const ArchiveEntry& entry, uint64_t block, const uint8_t* src, size_t srcLen, bool raw,
							uint8_t* dst);

	/**
	 * Offline check of each file's content hash (not checked by default at runtime).
	 * @param cb Called after each file entry is checked (may be empty)
	 * @return OK if every file passes; CORRUPTED if a file hash mismatches or the data is corrupt; the first read error (for example IO_ERROR) if a read fails
	 */
	static Error verify(const ArchiveIndex& index,
						const std::function<void(const ArchiveEntry& entry, Error result)>& cb = nullptr);
};

/**
 * Read exactly size bytes from an absolute offset of the source stream (loops to handle short reads).
 */
Error readFully(IFileStream& stream, int64_t offset, uint8_t* buf, size_t size);

NS_VFS_END
