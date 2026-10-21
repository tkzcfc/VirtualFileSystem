// This file: a read-only stream for one uncompressed file in an archive. Plaintext and XOR share one class.
#pragma once

#include "../Common.h"
#include "../IFileStream.h"
#include "ArchiveReader.h"

NS_VFS_BEGIN

/**
 * Read-only stream for a single file in an archive.
 * When the source supports positional reads (native file, memory), IArchiveSource::readAt is called directly and the stream holds no OS handle;
 * otherwise it holds its own source stream and reads by offset. Neither path takes a lock.
 * - If the source is shorter than the index says (the archive was truncated or replaced while mounted): read returns -1 (errno=EIO) and truncated data is not treated as a complete file.
 * - seek only changes m_pos; out-of-range rules match POSIX.
 * - read: clamp to [0, size), then read from the underlying handle at base+offset+pos; XOR files are decrypted in place by position.
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

	std::shared_ptr<const ArchiveIndex> m_index;   // keeps the index and key alive
	const ArchiveEntry* m_entry;                   // current entry
	std::unique_ptr<IFileStream> m_file;           // independent handle (when the source does not support positional reads)
	bool m_open;                                   // false after close()
	int64_t m_dataStart;                           // absolute offset of the stored data in the source
	mutable int64_t m_pos;                         // current position (const because the axmol interface requires it)
	mutable int64_t m_filePos;                     // current position of the underlying handle, so sequential reads can skip seek; -1 means unknown
};

NS_VFS_END
