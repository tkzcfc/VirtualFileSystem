// This file: a read-only stream for one file in an archive. Plaintext, XOR, and block-LZ4 entries share one class.
#pragma once

#include "../Common.h"
#include "../IFileStream.h"
#include "ArchiveReader.h"
#include <vector>

NS_VFS_BEGIN

/**
 * Read-only stream for a single file in an archive.
 * When the source supports positional reads (native file, memory), IArchiveSource::readAt is called directly and the stream holds no OS handle;
 * otherwise it holds its own source stream and reads by offset. Neither path takes a lock.
 * - If the source is shorter than the index says (the archive was truncated or replaced while mounted): read returns -1 (errno=EIO) and truncated data is not treated as a complete file.
 * - seek only changes m_pos; out-of-range rules match POSIX.
 * - read: clamp to [0, size), then read from the underlying handle at base+offset+pos; XOR files are decrypted in place by position.
 * - LZ4 entries: the block table is loaded on the first read; each read decompresses only the blocks it touches, and only the
 *   current decompressed block is cached. seek stays O(1). A corrupt block or table makes read return -1 (errno=EIO).
 * - write returns -1 (errno=EROFS), resize returns false (errno=ENOTSUP), and nativeHandle() returns -1.
 * The same object must not be used across threads.
 */
class VFS_DLL ArchiveFileStream : public IFileStream
{
public:

	/**
	 * @param index archive index (shared ownership; keeps the index and key alive for lazy unmount)
	 * @param entry entry pointing inside index; its lifetime is guaranteed by index
	 * @param file  independent source stream; when null, index->source->readAt is used (the source must supportsReadAt())
	 */
	ArchiveFileStream(std::shared_ptr<const ArchiveIndex> index, const ArchiveEntry* entry,
					  std::unique_ptr<IFileStream> file);

	virtual ~ArchiveFileStream();

	virtual bool open(std::string_view path, IFileStream::Mode mode) override;

	virtual int close() override;

	virtual int64_t seek(int64_t offset, int origin) const override;

	/**
	 * Read data. Encrypted files are decrypted in place by position.
	 * @param buf  output buffer
	 * @param size number of bytes requested
	 * @return number of bytes actually read, 0 at EOF, -1 on error (errno is set)
	 */
	virtual int read(void* buf, unsigned int size) const override;

	virtual int write(const void* buf, unsigned int size) const override;

	virtual bool resize(int64_t size) const override;

	virtual int64_t tell() const override;

	virtual int64_t size() const override;

	virtual bool isOpen() const override;

private:
	// Read from absolute source offset target; returns the byte count or -1
	int64_t readSource(uint8_t* out, size_t want, int64_t target) const;

	// Read exactly len stored bytes at storedOff (relative to the entry's stored data) and XOR-decrypt them; false on error
	bool readStored(uint8_t* out, size_t len, uint64_t storedOff) const;

	// LZ4: make block the current block (load the table on first use); false on error (errno set)
	bool loadBlock(uint64_t block) const;

	// LZ4: read up to want bytes at m_pos through the block cache; returns the byte count or -1
	int64_t readCompressed(uint8_t* out, size_t want) const;

	std::shared_ptr<const ArchiveIndex> m_index;   // keeps the index and key alive
	const ArchiveEntry* m_entry;                   // current entry
	std::unique_ptr<IFileStream> m_file;           // independent handle (when the source does not support positional reads)
	bool m_open;                                   // false after close()
	int64_t m_dataStart;                           // absolute offset of the stored data in the source
	mutable int64_t m_pos;                         // current position (const because the axmol interface requires it)
	mutable int64_t m_filePos;                     // current position of the underlying handle, so sequential reads can skip seek; -1 means unknown

	// LZ4 state (unused for uncompressed entries)
	mutable std::vector<uint64_t> m_blockTable;    // decrypted table items, loaded lazily
	mutable std::vector<uint8_t> m_block;          // current decompressed block
	mutable std::vector<uint8_t> m_scratch;        // stored bytes of the block being decoded
	mutable uint64_t m_curBlock;                   // block held in m_block; UINT64_MAX = none
};

NS_VFS_END
