#include "NativeFileStream.h"
#include <climits>

#if VFS_PLATFORM_WINDOWS
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#    include "NativeUtils.h"
#else
#    include <sys/stat.h>
#    include <sys/types.h>
#    include <unistd.h>
#    include <fcntl.h>
#    include <errno.h>
// Creation permissions matching axmol: user and group can read and write
#    define VFS_O_SHARE_FLAGS (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)
#    if defined(O_CLOEXEC)
#        define VFS_O_CLOEXEC O_CLOEXEC
#    else
#        define VFS_O_CLOEXEC 0
#    endif
// Always use lseek / ftruncate: the library is built with _FILE_OFFSET_BITS=64 (see src/vfs/CMakeLists.txt), so off_t is 64-bit even on 32-bit glibc;
// musl dropped lseek64 / ftruncate64 starting with 1.2.4, and Apple and BSD off_t was already 64-bit
static_assert(sizeof(off_t) == 8, "need 64-bit off_t: build with _FILE_OFFSET_BITS=64");
#    define vfs_lseek64 ::lseek
#    define vfs_ftruncate64 ::ftruncate
#endif

NS_VFS_BEGIN

#if VFS_PLATFORM_WINDOWS
static const osfhnd_t s_invalidHandle = INVALID_HANDLE_VALUE;
#else
static const osfhnd_t s_invalidHandle = -1;
#endif

NativeFileStream::NativeFileStream()
	: m_handle(s_invalidHandle)
	, m_append(false)
	, m_writeOnly(false)
{
}

NativeFileStream::NativeFileStream(NativeFileStream&& other) noexcept
	: m_handle(other.m_handle)
	, m_append(other.m_append)
	, m_writeOnly(other.m_writeOnly)
{
	other.m_handle = s_invalidHandle;
	other.m_append = false;
	other.m_writeOnly = false;
}

NativeFileStream& NativeFileStream::operator=(NativeFileStream&& other) noexcept
{
	if (this != &other)
	{
		close();
		m_handle = other.m_handle;
		m_append = other.m_append;
		m_writeOnly = other.m_writeOnly;
		other.m_handle = s_invalidHandle;
		other.m_append = false;
		other.m_writeOnly = false;
	}
	return *this;
}

NativeFileStream::~NativeFileStream()
{
	close();
}

#if VFS_PLATFORM_WINDOWS

namespace
{
// errno when ReadFile / WriteFile / SetEndOfFile fails: a full disk reports ENOSPC (same as POSIX write),
// permission and sharing conflicts are EACCES, an invalid handle is EBADF, and everything else is an I/O error
int ioErrno(DWORD code)
{
	switch (code)
	{
	case ERROR_DISK_FULL:
	case ERROR_HANDLE_DISK_FULL:
		return ENOSPC;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
		return EACCES;
	case ERROR_INVALID_HANDLE:
		return EBADF;
	default:
		return EIO;
	}
}
} // namespace

bool NativeFileStream::open(std::string_view path, IFileStream::Mode mode)
{
	close();

	// Same mode table as axmol, except the share mode also includes FILE_SHARE_DELETE: other handles may delete, rename, or replace this file (POSIX behavior)
	static const DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
	DWORD access = GENERIC_READ | GENERIC_WRITE;
	DWORD creation = OPEN_ALWAYS;
	switch (mode)
	{
	case Mode::READ:
		access = GENERIC_READ;
		creation = OPEN_EXISTING;
		break;
	case Mode::WRITE:
		creation = CREATE_ALWAYS;
		break;
	case Mode::APPEND:
		// No read access yet: opening a just-written file with read access makes Windows Defender scan it
		// synchronously (about 5 ms per open), which dominates open/append/close loops such as logs.
		// The handle is reopened with read access on the first read (ensureReadable).
		access = GENERIC_WRITE | FILE_READ_ATTRIBUTES;
		creation = OPEN_ALWAYS;
		break;
	case Mode::OVERLAPPED:
	default:
		creation = OPEN_ALWAYS;
		break;
	}

	std::wstring wpath = native::toWidePath(path);
	HANDLE h = INVALID_HANDLE_VALUE;
	// Antivirus / the indexer may briefly open the file without sharing, and a same-named file left "delete pending" can also make create fail, so retry a limited number of times.
	// ACCESS_DENIED from opening a directory is not transient and is not retried
	DWORD code = native::retryTransient([&]() -> unsigned long
	{
		h = ::CreateFileW(wpath.c_str(), access, shareMode, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h != INVALID_HANDLE_VALUE)
			return ERROR_SUCCESS;
		DWORD c = ::GetLastError();
		if (c == ERROR_ACCESS_DENIED)
		{
			DWORD attr = ::GetFileAttributesW(wpath.c_str());
			if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
				return ERROR_DIRECTORY_NOT_SUPPORTED;
		}
		return c;
	});
	if (h == INVALID_HANDLE_VALUE)
	{
		if (code == ERROR_DIRECTORY_NOT_SUPPORTED)
			errno = EISDIR;
		else if (code == ERROR_PATH_NOT_FOUND)
			errno = ENOENT;
		else
			errno = toErrno(native::errorFromWin32(code));
		return false;
	}
	m_handle = h;
	m_append = mode == Mode::APPEND;
	m_writeOnly = m_append;
	return true;
}

bool NativeFileStream::ensureReadable() const
{
	if (!m_writeOnly)
		return true;
	// Same file, same sharing, now with read access; the file pointer moves over to the new handle
	LARGE_INTEGER zero{}, pos{};
	if (!::SetFilePointerEx(m_handle, zero, &pos, FILE_CURRENT))
	{
		errno = EIO;
		return false;
	}
	HANDLE h = ::ReOpenFile(m_handle, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
	if (h == INVALID_HANDLE_VALUE)
	{
		errno = ioErrno(::GetLastError());
		return false;
	}
	::CloseHandle(m_handle);
	m_handle = h;
	m_writeOnly = false;
	::SetFilePointerEx(m_handle, pos, nullptr, FILE_BEGIN);
	return true;
}

int NativeFileStream::close()
{
	if (m_handle != s_invalidHandle)
	{
		::CloseHandle(m_handle);
		m_handle = s_invalidHandle;
	}
	m_append = false;
	m_writeOnly = false;
	return 0;
}

int64_t NativeFileStream::seek(int64_t offset, int origin) const
{
	if (m_handle == s_invalidHandle)
	{
		errno = EBADF;
		return -1;
	}
	// SEEK_SET/SEEK_CUR/SEEK_END have the same numeric values as FILE_BEGIN/FILE_CURRENT/FILE_END
	LARGE_INTEGER seekpos, newpos;
	seekpos.QuadPart = offset;
	if (::SetFilePointerEx(m_handle, seekpos, &newpos, static_cast<DWORD>(origin)))
		return newpos.QuadPart;
	errno = EINVAL;
	return -1;
}

int NativeFileStream::read(void* buf, unsigned int size) const
{
	if (m_handle == s_invalidHandle)
	{
		errno = EBADF;
		return -1;
	}
	if (size > VFS_MAX_IO_CHUNK)
		size = VFS_MAX_IO_CHUNK;
	if (!ensureReadable())
		return -1;
	DWORD bytesRead = 0;
	if (::ReadFile(m_handle, buf, size, &bytesRead, nullptr))
		return static_cast<int>(bytesRead);
	errno = ioErrno(::GetLastError());
	return -1;
}

int NativeFileStream::write(const void* buf, unsigned int size) const
{
	if (m_handle == s_invalidHandle)
	{
		errno = EBADF;
		return -1;
	}
	if (size > VFS_MAX_IO_CHUNK)
		size = VFS_MAX_IO_CHUNK;
	DWORD bytesWritten = 0;
	if (m_append)
	{
		// Windows has no O_APPEND. "Seek to the end, then write" overwrites itself when several handles append at once;
		// an OVERLAPPED offset of all 0xFFFFFFFF means "write at the end of the file", done atomically by the system, equivalent to FILE_APPEND_DATA.
		// On a synchronous handle the file pointer sits just after the written data
		OVERLAPPED ov{};
		ov.Offset = 0xFFFFFFFFu;
		ov.OffsetHigh = 0xFFFFFFFFu;
		if (::WriteFile(m_handle, buf, size, &bytesWritten, &ov))
			return static_cast<int>(bytesWritten);
		errno = ioErrno(::GetLastError());
		return -1;
	}
	if (::WriteFile(m_handle, buf, size, &bytesWritten, nullptr))
		return static_cast<int>(bytesWritten);
	errno = ioErrno(::GetLastError());
	return -1;
}

int64_t NativeFileStream::size() const
{
	if (m_handle == s_invalidHandle)
	{
		errno = EBADF;
		return -1;
	}
	LARGE_INTEGER endpos;
	if (::GetFileSizeEx(m_handle, &endpos))
		return endpos.QuadPart;
	errno = EIO;
	return -1;
}

bool NativeFileStream::resize(int64_t newSize) const
{
	if (m_handle == s_invalidHandle || newSize < 0)
	{
		errno = m_handle == s_invalidHandle ? EBADF : EINVAL;
		return false;
	}
	// SetEndOfFile uses the current position as the new end, so save the position and restore it afterwards (same as axmol's seek_guard)
	int64_t place = tell();
	if (seek(newSize, SEEK_SET) < 0)
		return false;
	bool ok = ::SetEndOfFile(m_handle) != FALSE;
	const DWORD code = ok ? ERROR_SUCCESS : ::GetLastError();
	seek(place, SEEK_SET);
	if (!ok)
		errno = ioErrno(code);
	return ok;
}

#else

bool NativeFileStream::open(std::string_view path, IFileStream::Mode mode)
{
	close();

	// POSIX flag table matching axmol; O_CLOEXEC is added so the handle is not leaked to child processes
	int flags = O_RDONLY;
	switch (mode)
	{
	case Mode::READ:
		flags = O_RDONLY;
		break;
	case Mode::WRITE:
		flags = O_CREAT | O_RDWR | O_TRUNC;
		break;
	case Mode::APPEND:
		flags = O_APPEND | O_CREAT | O_RDWR;
		break;
	case Mode::OVERLAPPED:
	default:
		flags = O_CREAT | O_RDWR;
		break;
	}

	std::string cpath(path);
	int fd = ::open(cpath.c_str(), flags | VFS_O_CLOEXEC, VFS_O_SHARE_FLAGS);
	if (fd == -1)
		return false;

	// POSIX allows opening a directory read-only; reject that here so every backend behaves the same
	struct stat st;
	if (::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode))
	{
		::close(fd);
		errno = EISDIR;
		return false;
	}

	m_handle = fd;
	m_append = mode == Mode::APPEND;
	return true;
}

int NativeFileStream::close()
{
	int fd = m_handle;
	m_append = false;
	if (fd != -1)
	{
		m_handle = -1;
		return ::close(fd);
	}
	return 0;
}

int64_t NativeFileStream::seek(int64_t offset, int origin) const
{
	if (m_handle == -1)
	{
		errno = EBADF;
		return -1;
	}
	return static_cast<int64_t>(vfs_lseek64(m_handle, offset, origin));
}

int NativeFileStream::read(void* buf, unsigned int size) const
{
	if (m_handle == -1)
	{
		errno = EBADF;
		return -1;
	}
	if (size > VFS_MAX_IO_CHUNK)
		size = VFS_MAX_IO_CHUNK;
	// Retry when interrupted by a signal (EINTR) instead of handing it to the caller as an I/O error
	ssize_t n;
	do
		n = ::read(m_handle, buf, size);
	while (n < 0 && errno == EINTR);
	return static_cast<int>(n);
}

int NativeFileStream::write(const void* buf, unsigned int size) const
{
	if (m_handle == -1)
	{
		errno = EBADF;
		return -1;
	}
	if (size > VFS_MAX_IO_CHUNK)
		size = VFS_MAX_IO_CHUNK;
	ssize_t n;
	do
		n = ::write(m_handle, buf, size);
	while (n < 0 && errno == EINTR);
	return static_cast<int>(n);
}

int64_t NativeFileStream::size() const
{
	if (m_handle == -1)
	{
		errno = EBADF;
		return -1;
	}
	// fstat does not change the current position, one system call fewer than axmol's two lseeks
	struct stat st;
	if (::fstat(m_handle, &st) != 0)
		return -1;
	return static_cast<int64_t>(st.st_size);
}

bool NativeFileStream::resize(int64_t newSize) const
{
	if (m_handle == -1 || newSize < 0)
	{
		errno = m_handle == -1 ? EBADF : EINVAL;
		return false;
	}
	// ftruncate does not move the file position; POSIX allows a position past the new size (a later read returns 0)
	return vfs_ftruncate64(m_handle, newSize) == 0;
}

#endif

int64_t NativeFileStream::tell() const
{
	return seek(0, SEEK_CUR);
}

bool NativeFileStream::isOpen() const
{
	return m_handle != s_invalidHandle;
}

osfhnd_t NativeFileStream::nativeHandle() const
{
	return m_handle;
}

NS_VFS_END
