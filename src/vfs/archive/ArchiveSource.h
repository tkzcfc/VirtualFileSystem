// This file: the abstraction for an archive's data source. An archive may come from a native file or from memory (for example embedded in the executable).
#pragma once

#include "../IFileStream.h"
#include "../Error.h"
#include "../memory/MemoryNode.h"

NS_VFS_BEGIN

/**
 * Archive data source.
 * Thread safety: openStream / readAt may be called concurrently from multiple threads.
 * - A source that supports positional reads (supportsReadAt() is true): file streams inside the archive read through readAt, opening a file makes no system call,
 *   and multiple threads reading the same archive do not lock each other;
 * - Other sources: each stream calls openStream to obtain an independent read-only stream (its own OS handle or its own cursor).
 */
class VFS_DLL IArchiveSource
{
public:
	virtual ~IArchiveSource() = default;

	/**
	 * Open a new read-only stream, positioned at 0.
	 */
	virtual std::unique_ptr<IFileStream> openStream(Error* err) const = 0;

	/**
	 * Whether readAt is supported. When this returns true, readAt must be callable from multiple threads at once.
	 */
	virtual bool supportsReadAt() const { return false; }

	/**
	 * Positional read: read at most size bytes from offset, without changing any shared state.
	 * @return The number of bytes actually read (may be less than size at end of file), or -1 on error (errno is set). The default implementation does not support this and returns -1 (ENOTSUP).
	 */
	virtual int64_t readAt(void* buf, size_t size, uint64_t offset) const;

	/**
	 * Modification time of the source (Unix seconds), used in place of an entry mtime of 0.
	 */
	virtual int64_t mtime() const { return 0; }

	/**
	 * Description (native path or "<memory>"), used in logs.
	 */
	virtual std::string describe() const = 0;
};

namespace native
{
class PositionalReader;
}

/**
 * Native file source: the constructor opens one shared read-only handle, and every stream reads through it by position (POSIX pread / Windows overlapped ReadFile),
 * so opening a file inside the archive does not open another OS handle. The handle allows other processes to delete, rename, or replace the archive file.
 * If the shared handle fails to open, fall back to "open a new handle on every openStream".
 */
class VFS_DLL FileArchiveSource : public IArchiveSource
{
public:

	explicit FileArchiveSource(std::string nativePath);

	virtual ~FileArchiveSource();

	virtual std::unique_ptr<IFileStream> openStream(Error* err) const override;

	virtual bool supportsReadAt() const override;

	virtual int64_t readAt(void* buf, size_t size, uint64_t offset) const override;

	virtual int64_t mtime() const override;

	virtual std::string describe() const override { return m_nativePath; }

private:
	std::string m_nativePath;
	int64_t m_mtime;   // Recorded at construction so it stays stable for one mount
	std::unique_ptr<native::PositionalReader> m_reader;   // Shared positional-read handle (null if open failed)
};

/**
 * Memory source: the data is shared by shared_ptr, each openStream returns a read-only memory stream with its own cursor, and readAt copies directly.
 * The data must not be modified while the archive is mounted.
 */
class VFS_DLL MemoryArchiveSource : public IArchiveSource
{
public:

	explicit MemoryArchiveSource(std::vector<uint8_t> data);

	explicit MemoryArchiveSource(std::shared_ptr<MemoryBuffer> buffer);

	virtual std::unique_ptr<IFileStream> openStream(Error* err) const override;

	virtual bool supportsReadAt() const override { return true; }

	virtual int64_t readAt(void* buf, size_t size, uint64_t offset) const override;

	virtual std::string describe() const override { return "<memory>"; }

private:
	std::shared_ptr<MemoryBuffer> m_buffer;
};

NS_VFS_END
