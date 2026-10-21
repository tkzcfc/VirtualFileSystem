// This file: writing a .vfs archive (shared by the CLI and applications).
// Flow: placeholder header -> write data in path order (alignment, dedup, optional compression/encryption) -> write the index -> write the footer -> backfill the header.
#pragma once

#include "ArchiveFormat.h"
#include "../Error.h"
#include "../IFileStream.h"

NS_VFS_BEGIN

/**
 * Archive-level write options.
 */
struct ArchiveWriteOptions
{
	uint32_t alignment = ArchiveFormat::DEFAULT_ALIGNMENT;   // data alignment, a power of two; 1 means unaligned
	bool encryptIndex = true;                                // encrypt the index region with ChaCha20
	std::vector<uint8_t> indexKey;                           // empty: derive deterministically from the index contents and store it in the file; otherwise it must be 32 bytes
	bool storeKey = false;                                   // when indexKey is non-empty, whether to store it in the file as well
	uint32_t fileKeyLength = ArchiveFormat::DEFAULT_FILE_KEY_SIZE;  // per-file XOR key length, 1..32
	bool dedup = true;                                       // files with identical contents are stored once
	int compressionLevel = 9;                                // LZ4 HC level 1..12
	uint8_t blockLog2 = ArchiveFormat::DEFAULT_BLOCK_LOG2;   // default LZ4 block size as log2 (MIN_BLOCK_LOG2..MAX_BLOCK_LOG2)
};

/**
 * Per-file write options (decided by the pack rules).
 */
struct FileWriteOptions
{
	bool encrypt = false;    // XOR encryption
	bool compress = false;   // LZ4 compression (if the compressed form is not smaller, store the original)
	uint8_t blockLog2 = 0;   // LZ4 block size as log2 for this file; 0 = ArchiveWriteOptions::blockLog2
	int64_t mtime = 0;       // mtime written into the index; default 0 keeps the output reproducible
};

/**
 * Write statistics.
 */
struct ArchiveWriteStats
{
	uint32_t fileCount = 0;        // number of file entries
	uint32_t dirCount = 0;         // number of empty-directory entries
	uint32_t dedupCount = 0;       // entries that share data with an earlier file
	uint32_t encryptedCount = 0;   // number of XOR-encrypted files
	uint32_t compressedCount = 0;  // number of files actually stored compressed
	uint64_t totalSize = 0;        // total raw content bytes
	uint64_t storedSize = 0;       // bytes actually written in the data region (excluding padding)
	uint64_t archiveSize = 0;      // total archive bytes
};

/**
 * Archive writer.
 * Output is reproducible: entries and data are ordered by path bytes, mtime defaults to 0, and each file's XOR key and the index key/nonce
 * are derived deterministically from the contents, so the same input is byte-for-byte identical every time.
 * Thread safety: a single object must not be used across threads.
 */
class VFS_DLL ArchiveWriter
{
public:

	explicit ArchiveWriter(const ArchiveWriteOptions& options = {});

	~ArchiveWriter();

	/**
	 * Add a native file. The contents are read only at write().
	 * @param archivePath path inside the archive (normalized to a relative path with no leading '/')
	 * @param nativePath  native file path (UTF-8)
	 */
	Error addFile(std::string_view archivePath, std::string_view nativePath, const FileWriteOptions& options = {});

	/**
	 * Add in-memory data.
	 */
	Error addData(std::string_view archivePath, std::vector<uint8_t> data, const FileWriteOptions& options = {});

	/**
	 * Add a directory. Only a directory that ends up empty is written as a Dir entry; non-empty directories are implied by file paths.
	 */
	Error addDir(std::string_view archivePath);

	size_t pendingCount() const { return m_pending.size(); }

	/**
	 * Write to a native file: write out.tmp first, then rename on success, so the result is atomic.
	 */
	Error write(std::string_view outPath, ArchiveWriteStats* stats = nullptr);

	/**
	 * Write to any readable, writable, seekable stream (starting at position 0).
	 */
	Error write(IFileStream& out, ArchiveWriteStats* stats = nullptr);

	/**
	 * Description of the most recent failure (for example a conflicting path), for CLI messages.
	 */
	const std::string& lastError() const { return m_lastError; }

private:
	struct Pending
	{
		std::string path;                              // normalized relative path
		EntryType type = EntryType::FILE;
		std::string nativePath;                        // native file (one of nativePath or data)
		std::shared_ptr<std::vector<uint8_t>> data;    // in-memory data
		FileWriteOptions options;
	};

	// Sort and check duplicates, case conflicts, and file/directory conflicts; drop Dir entries for non-empty directories
	Error prepare(std::vector<Pending>& sorted);

	Error fail(Error err, std::string message);

private:
	ArchiveWriteOptions m_options;
	std::vector<Pending> m_pending;
	std::string m_lastError;
};

NS_VFS_END
