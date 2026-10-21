// This file: the memory file stream. Used both by the memory backend and for decompressed data of compressed files in an archive (read-only mode).
#pragma once

#include "../IFileStream.h"
#include "MemoryNode.h"

NS_VFS_BEGIN

/**
 * Memory file stream.
 * - Constructed by the backend factory. The public open(path, mode) returns false and sets errno=ENOTSUP.
 * - In READ mode, write/resize fail (errno=EBADF).
 * - APPEND mode writes to the end on every write; a write after a seek past the end fills the hole with 0.
 * Thread safety: a single stream object must not be used across threads; when several streams share one buffer, the buffer's lock keeps memory access safe.
 */
class VFS_DLL MemoryFileStream : public IFileStream
{
public:

	/**
	 * @param buffer Data buffer (shared ownership)
	 * @param mode   Open mode; truncation (WRITE) is done by the caller before construction
	 */
	MemoryFileStream(std::shared_ptr<MemoryBuffer> buffer, IFileStream::Mode mode);

	virtual ~MemoryFileStream();

	virtual bool open(std::string_view path, IFileStream::Mode mode) override;

	virtual int close() override;

	virtual int64_t seek(int64_t offset, int origin) const override;

	virtual int read(void* buf, unsigned int size) const override;

	virtual int write(const void* buf, unsigned int size) const override;

	virtual int64_t tell() const override;

	virtual int64_t size() const override;

	virtual bool resize(int64_t size) const override;

	virtual bool isOpen() const override;

private:
	std::shared_ptr<MemoryBuffer> m_buffer;   // Cleared after close
	IFileStream::Mode m_mode;
	mutable int64_t m_pos;                    // Current position (the axmol interface requires const methods)
};

NS_VFS_END
