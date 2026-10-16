#include "NativeUtils.h"

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#else
#    include <sys/stat.h>
#    include <errno.h>
#endif

NS_VFS_BEGIN

namespace native
{

std::filesystem::path toFsPath(std::string_view utf8)
{
#if defined(_WIN32)
	// Windows 上直接用 UTF-16，顺便处理长路径前缀
	return std::filesystem::path(toWidePath(utf8));
#else
	std::u8string u8(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
	return std::filesystem::path(u8);
#endif
}

std::string fromFsPath(const std::filesystem::path& p)
{
	std::u8string u8 = p.u8string();
	return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

Error errorFromErrno(int e)
{
	switch (e)
	{
	case 0:
		return Error::OK;
	case ENOENT:
		return Error::NOT_FOUND;
	case EEXIST:
		return Error::EXISTS;
	case ENOTDIR:
		return Error::NOT_DIR;
	case EISDIR:
		return Error::IS_DIR;
	case ENOTEMPTY:
		return Error::NOT_EMPTY;
#if defined(EROFS)
	case EROFS:
		return Error::READ_ONLY;
#endif
	case EXDEV:
		return Error::CROSS_DEVICE;
	case EBUSY:
		return Error::BUSY;
	case EACCES:
	case EPERM:
		return Error::PERMISSION_DENIED;
	case EINVAL:
	case ENAMETOOLONG:
		return Error::INVALID_PATH;
	default:
		return Error::IO_ERROR;
	}
}

Error errorFromCode(const std::error_code& ec)
{
	if (!ec)
		return Error::OK;
	// std::filesystem 在各平台上都把错误映射到 generic_category 的 errc，统一按 errno 处理
	std::error_condition cond = ec.default_error_condition();
	if (cond.category() == std::generic_category())
		return errorFromErrno(cond.value());
	return Error::IO_ERROR;
}

#if defined(_WIN32)

std::wstring toWidePath(std::string_view utf8)
{
	std::wstring wide;
	if (!utf8.empty())
	{
		int len = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
		if (len > 0)
		{
			wide.resize(static_cast<size_t>(len));
			::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), len);
		}
	}
	for (auto& ch : wide)
	{
		if (ch == L'/')
			ch = L'\\';
	}
	// 绝对路径（"C:\\..."）超过 MAX_PATH 时加长路径前缀，否则 Win32 API 会失败
	if (wide.size() >= MAX_PATH && wide.size() > 2 && wide[1] == L':' && wide.compare(0, 4, L"\\\\?\\") != 0)
		wide.insert(0, L"\\\\?\\");
	return wide;
}

static int64_t fileTimeToUnix(const FILETIME& ft)
{
	// FILETIME 是 1601-01-01 起的 100ns 计数
	ULARGE_INTEGER v;
	v.LowPart = ft.dwLowDateTime;
	v.HighPart = ft.dwHighDateTime;
	return static_cast<int64_t>(v.QuadPart / 10000000ULL) - 11644473600LL;
}

Error statPath(std::string_view utf8Path, Stat& out)
{
	std::wstring wpath = toWidePath(utf8Path);
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!::GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &data))
	{
		DWORD code = ::GetLastError();
		if (code == ERROR_FILE_NOT_FOUND)
			return Error::NOT_FOUND;
		if (code == ERROR_PATH_NOT_FOUND || code == ERROR_DIRECTORY)
		{
			// Windows 不区分“父目录不存在”和“父路径是文件”，这里逐级向上检查（只在未命中时发生）
			std::string_view p = utf8Path;
			while (true)
			{
				size_t pos = p.find_last_of("/\\");
				if (pos == std::string_view::npos || pos == 0)
					break;
				p = p.substr(0, pos);
				DWORD attr = ::GetFileAttributesW(toWidePath(p).c_str());
				if (attr != INVALID_FILE_ATTRIBUTES)
					return (attr & FILE_ATTRIBUTE_DIRECTORY) ? Error::NOT_FOUND : Error::NOT_DIR;
			}
			return Error::NOT_FOUND;
		}
		if (code == ERROR_ACCESS_DENIED)
			return Error::PERMISSION_DENIED;
		if (code == ERROR_INVALID_NAME || code == ERROR_BAD_PATHNAME)
			return Error::INVALID_PATH;
		return Error::IO_ERROR;
	}
	out.type = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? FileType::DIR : FileType::FILE;
	out.size = out.type == FileType::FILE
				   ? static_cast<int64_t>((static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow)
				   : 0;
	out.mtime = fileTimeToUnix(data.ftLastWriteTime);
	return Error::OK;
}

#else

Error statPath(std::string_view utf8Path, Stat& out)
{
	std::string path(utf8Path);
	struct stat st;
	// stat 会跟随 symlink；是否允许跟随由 NativeBackend 另行检查
	if (::stat(path.c_str(), &st) != 0)
		return errorFromErrno(errno);
	if (S_ISDIR(st.st_mode))
	{
		out.type = FileType::DIR;
		out.size = 0;
	}
	else if (S_ISREG(st.st_mode))
	{
		out.type = FileType::FILE;
		out.size = static_cast<int64_t>(st.st_size);
	}
	else
	{
		// 设备、管道、socket 等特殊文件不暴露给 VFS
		return Error::NOT_FOUND;
	}
	out.mtime = static_cast<int64_t>(st.st_mtime);
	return Error::OK;
}

#endif

} // namespace native

NS_VFS_END
