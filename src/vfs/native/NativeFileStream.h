// This file: native file streams, following axmol `FileStream`'s lowio implementation.
// POSIX uses open/lseek64/read/write; Windows uses the CreateFileW family (UTF-8 converted to UTF-16).
#pragma once

#include "../Common.h"
#include "../IFileStream.h"

NS_VFS_BEGIN

/**
 * Native file stream.
 * - Same open semantics as axmol FileStream: READ=O_RDONLY; WRITE=O_CREAT|O_RDWR|O_TRUNC;
 *   APPEND=O_APPEND|O_CREAT|O_RDWR; OVERLAPPED=O_CREAT|O_RDWR.
 * - On Windows, FILE_SHARE_DELETE is also set so the file may be deleted or replaced while open.
 * - Opening a directory fails (errno=EISDIR).
 * Thread safety: one object must not be used from multiple threads at once; distinct objects do not interfere. Movable only, not copyable.
 */
class VFS_DLL NativeFileStream : public IFileStream
{
public:

	NativeFileStream();

	NativeFileStream(NativeFileStream&& other) noexcept;

	NativeFileStream& operator=(NativeFileStream&& other) noexcept;

	NativeFileStream(const NativeFileStream&) = delete;

	NativeFileStream& operator=(const NativeFileStream&) = delete;

	virtual ~NativeFileStream();

	/**
	 * Open a native file.
	 * @param path native path in UTF-8
	 * @param mode open mode
	 * @return true on success; false on failure, with errno set
	 */
	virtual bool open(std::string_view path, IFileStream::Mode mode) override;

	virtual int close() override;

	virtual int64_t seek(int64_t offset, int origin) const override;

	virtual int read(void* buf, unsigned int size) const override;

	virtual int write(const void* buf, unsigned int size) const override;

	virtual int64_t tell() const override;

	virtual int64_t size() const override;

	virtual bool resize(int64_t size) const override;

	virtual bool isOpen() const override;

	// Windows APPEND streams have no read access until the first read(), which also replaces this handle
	virtual osfhnd_t nativeHandle() const override;

private:
	// Windows APPEND: reopen the handle with read access before the first read
	bool ensureReadable() const;

	mutable osfhnd_t m_handle;   // native handle; (osfhnd_t)-1 when not open
	bool m_append;               // APPEND mode: on Windows, seek to the end before every write
	mutable bool m_writeOnly;    // Windows APPEND: opened without read access until the first read (see open())
};

NS_VFS_END
