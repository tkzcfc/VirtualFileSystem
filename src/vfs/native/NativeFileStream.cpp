#include "NativeFileStream.h"
#include <climits>

#if defined(_WIN32)
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
// 与 axmol 一致的创建权限：用户和组可读写
#    define VFS_O_SHARE_FLAGS (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)
#    if defined(O_CLOEXEC)
#        define VFS_O_CLOEXEC O_CLOEXEC
#    else
#        define VFS_O_CLOEXEC 0
#    endif
// Apple、Emscripten、BSD 的 off_t 本身就是 64 位，没有 lseek64
#    if defined(__APPLE__) || defined(__EMSCRIPTEN__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#        define vfs_lseek64 ::lseek
#        define vfs_ftruncate64 ::ftruncate
#    else
#        define vfs_lseek64 ::lseek64
#        define vfs_ftruncate64 ::ftruncate64
#    endif
#endif

NS_VFS_BEGIN

#if defined(_WIN32)
static const osfhnd_t s_invalidHandle = INVALID_HANDLE_VALUE;
#else
static const osfhnd_t s_invalidHandle = -1;
#endif

NativeFileStream::NativeFileStream()
	: m_handle(s_invalidHandle)
	, m_append(false)
{
}

NativeFileStream::NativeFileStream(NativeFileStream&& other) noexcept
	: m_handle(other.m_handle)
	, m_append(other.m_append)
{
	other.m_handle = s_invalidHandle;
	other.m_append = false;
}

NativeFileStream& NativeFileStream::operator=(NativeFileStream&& other) noexcept
{
	if (this != &other)
	{
		close();
		m_handle = other.m_handle;
		m_append = other.m_append;
		other.m_handle = s_invalidHandle;
		other.m_append = false;
	}
	return *this;
}

NativeFileStream::~NativeFileStream()
{
	close();
}

#if defined(_WIN32)

bool NativeFileStream::open(std::string_view path, IFileStream::Mode mode)
{
	close();

	// 与 axmol 相同的模式表，只是共享模式多了 FILE_SHARE_DELETE：其他句柄可以删除、改名、替换这个文件（POSIX 行为）
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
	case Mode::OVERLAPPED:
	default:
		creation = OPEN_ALWAYS;
		break;
	}

	std::wstring wpath = native::toWidePath(path);
	HANDLE h = INVALID_HANDLE_VALUE;
	// 杀毒软件 / 索引服务可能短暂地以不共享的方式打开文件，“删除挂起”的同名文件也会让创建失败，做有限次重试。
	// 打开目录返回的 ACCESS_DENIED 不是瞬时错误，不重试
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
	return 0;
}

int64_t NativeFileStream::seek(int64_t offset, int origin) const
{
	if (m_handle == s_invalidHandle)
	{
		errno = EBADF;
		return -1;
	}
	// SEEK_SET/SEEK_CUR/SEEK_END 与 FILE_BEGIN/FILE_CURRENT/FILE_END 的数值相同
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
	DWORD bytesRead = 0;
	if (::ReadFile(m_handle, buf, size, &bytesRead, nullptr))
		return static_cast<int>(bytesRead);
	errno = EACCES;
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
		// Windows 没有 O_APPEND。“先 seek 到末尾再写”在多个句柄同时追加时会互相覆盖；
		// OVERLAPPED 偏移全为 0xFFFFFFFF 表示“写到文件末尾”，由系统原子地完成，等价于 FILE_APPEND_DATA。
		// 同步句柄上写完后文件指针位于写入数据之后
		OVERLAPPED ov{};
		ov.Offset = 0xFFFFFFFFu;
		ov.OffsetHigh = 0xFFFFFFFFu;
		if (::WriteFile(m_handle, buf, size, &bytesWritten, &ov))
			return static_cast<int>(bytesWritten);
		errno = EACCES;
		return -1;
	}
	if (::WriteFile(m_handle, buf, size, &bytesWritten, nullptr))
		return static_cast<int>(bytesWritten);
	errno = EACCES;
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
	// SetEndOfFile 以当前位置为新的结尾，所以先保存位置，完成后恢复（与 axmol 的 seek_guard 一致）
	int64_t place = tell();
	if (seek(newSize, SEEK_SET) < 0)
		return false;
	bool ok = ::SetEndOfFile(m_handle) != FALSE;
	seek(place, SEEK_SET);
	if (!ok)
		errno = EACCES;
	return ok;
}

#else

bool NativeFileStream::open(std::string_view path, IFileStream::Mode mode)
{
	close();

	// 与 axmol 一致的 POSIX 标志表；额外加 O_CLOEXEC，避免句柄泄漏到子进程
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

	// POSIX 允许以只读方式打开目录，这里拒绝，保证所有后端行为一致
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
	return static_cast<int>(::read(m_handle, buf, size));
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
	return static_cast<int>(::write(m_handle, buf, size));
}

int64_t NativeFileStream::size() const
{
	if (m_handle == -1)
	{
		errno = EBADF;
		return -1;
	}
	// fstat 不改变当前位置，比 axmol 的两次 lseek 少一次系统调用
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
	// ftruncate 不移动文件位置；位置超过新大小时 POSIX 允许（之后 read 返回 0）
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
