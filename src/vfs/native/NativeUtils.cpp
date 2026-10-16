#include "NativeUtils.h"

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#    include <cstring>
#    include <cwchar>
#    include <vector>
#else
#    if defined(__APPLE__)
#        include <sys/attr.h>
#        include <cstring>
#    endif
#    include <sys/stat.h>
#    include <unistd.h>
#    include <errno.h>
#    include <cstdio>
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
	std::string s(reinterpret_cast<const char*>(u8.data()), u8.size());
#if defined(_WIN32)
	// 去掉长路径前缀，保证对外（日志、realPath、路径比较）始终是普通的 Win32 路径；两种分隔符都要处理
	auto sep = [](char c) { return c == '\\' || c == '/'; };
	if (s.size() >= 4 && sep(s[0]) && sep(s[1]) && s[2] == '?' && sep(s[3]))
	{
		if (s.size() >= 8 && (s.compare(4, 3, "UNC") == 0 || s.compare(4, 3, "unc") == 0) && sep(s[7]))
			s = s.substr(0, 2) + s.substr(8);          // \\?\UNC\server\share -> \\server\share
		else if (s.size() >= 6 && s[5] == ':')
			s = s.substr(4);                           // \\?\C:\... -> C:\...
	}
#endif
	return s;
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
#if defined(ELOOP)
	case ELOOP:
#endif
		return Error::INVALID_PATH;
	default:
		return Error::IO_ERROR;
	}
}

Error errorFromCode(const std::error_code& ec)
{
	if (!ec)
		return Error::OK;
#if defined(_WIN32)
	// Windows 上 std::filesystem 报告的多是 system_category 的 Win32 错误码，直接按 Win32 映射更准确
	if (ec.category() == std::system_category())
		return errorFromWin32(static_cast<unsigned long>(ec.value()));
#endif
	// 其余平台的 std::filesystem 都把错误映射到 generic_category 的 errc，统一按 errno 处理
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
	// 已经是 Win32 命名空间路径（\\?\ 或设备路径 \\.\），不再处理
	if (wide.compare(0, 4, L"\\\\?\\") == 0 || wide.compare(0, 4, L"\\\\.\\") == 0)
		return wide;
	// 短路径保持原样；达到 248（CreateDirectoryW 的限制是 MAX_PATH - 12）时转成带前缀的绝对路径。
	// 加了前缀后 Win32 不再规范化路径，所以必须先用 GetFullPathNameW 得到绝对、规范的形式
	if (wide.size() < MAX_PATH - 12)
		return wide;
	DWORD need = ::GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
	if (need == 0)
		return wide;
	std::wstring full(need, L'\0');
	DWORD got = ::GetFullPathNameW(wide.c_str(), need, full.data(), nullptr);
	if (got == 0 || got >= need)
		return wide;
	full.resize(got);
	if (full.compare(0, 2, L"\\\\") == 0)
		return L"\\\\?\\UNC\\" + full.substr(2);   // \\server\share\... -> \\?\UNC\server\share\...
	return L"\\\\?\\" + full;
}

Error errorFromWin32(unsigned long code)
{
	switch (code)
	{
	case ERROR_SUCCESS:
		return Error::OK;
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_DRIVE:
	case ERROR_BAD_NETPATH:
	case ERROR_BAD_NET_NAME:
		return Error::NOT_FOUND;
	case ERROR_FILE_EXISTS:
	case ERROR_ALREADY_EXISTS:
		return Error::EXISTS;
	case ERROR_DIRECTORY:
		return Error::NOT_DIR;
	case ERROR_DIR_NOT_EMPTY:
		return Error::NOT_EMPTY;
	case ERROR_WRITE_PROTECT:
		return Error::READ_ONLY;
	case ERROR_NOT_SAME_DEVICE:
		return Error::CROSS_DEVICE;
	case ERROR_BUSY:
	case ERROR_CURRENT_DIRECTORY:
		return Error::BUSY;
	case ERROR_INVALID_NAME:
	case ERROR_BAD_PATHNAME:
	case ERROR_FILENAME_EXCED_RANGE:
		return Error::INVALID_PATH;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:
	case ERROR_DELETE_PENDING:
	case ERROR_PRIVILEGE_NOT_HELD:
		return Error::PERMISSION_DENIED;
	case ERROR_NOT_SUPPORTED:
	case ERROR_INVALID_FUNCTION:
		return Error::UNSUPPORTED;
	default:
		return Error::IO_ERROR;
	}
}

bool isTransientWin32(unsigned long code)
{
	return code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION ||
		   code == ERROR_DELETE_PENDING;
}

unsigned long retryTransient(const std::function<unsigned long()>& fn)
{
	// 第一次不等待；之后 1、2、4、8、16、32ms，总共最多约 63ms。真正的权限错误也只多花这么多时间
	static const DWORD delays[] = {0, 1, 2, 4, 8, 16, 32};
	unsigned long code = ERROR_SUCCESS;
	for (DWORD d : delays)
	{
		if (d)
			::Sleep(d);
		code = fn();
		if (!isTransientWin32(code))
			break;
	}
	return code;
}

static int64_t fileTimeToUnix(const FILETIME& ft)
{
	// FILETIME 是 1601-01-01 起的 100ns 计数
	ULARGE_INTEGER v;
	v.LowPart = ft.dwLowDateTime;
	v.HighPart = ft.dwHighDateTime;
	return static_cast<int64_t>(v.QuadPart / 10000000ULL) - 11644473600LL;
}

// 路径不存在时区分“父目录不存在”（NOT_FOUND）和“某一级父路径是文件”（NOT_DIR）。
// Windows 对两者都返回 ERROR_PATH_NOT_FOUND，这里逐级向上检查（只在未命中时发生）
static Error classifyMissing(std::string_view utf8Path)
{
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

// 路径（或它的父目录）此刻是否已经不存在。“删除挂起”的对象逻辑上已被删除，也算不存在
static bool vanished(std::string_view utf8Path)
{
	if (::GetFileAttributesW(toWidePath(utf8Path).c_str()) != INVALID_FILE_ATTRIBUTES)
		return false;
	DWORD c = ::GetLastError();
	return c == ERROR_FILE_NOT_FOUND || c == ERROR_PATH_NOT_FOUND || c == ERROR_DELETE_PENDING;
}

static std::string_view parentOf(std::string_view utf8Path)
{
	size_t pos = utf8Path.find_last_of("/\\");
	return pos == std::string_view::npos || pos == 0 ? std::string_view() : utf8Path.substr(0, pos);
}

/**
 * Win32 错误码转 Error，并处理并发竞争：
 * - PATH_NOT_FOUND / DIRECTORY：区分 NOT_FOUND 与 NOT_DIR
 * - mustExist（操作的是已有对象：删除、改名源、列目录、stat）时，ACCESS_DENIED / SHARING_VIOLATION /
 *   DELETE_PENDING / INVALID_NAME 之后对象已经消失，说明是被其他线程/进程删掉或移走了，按 NOT_FOUND 报告
 *   （例如另一个线程正在改名同一个目录时，本次改名拿到 ACCESS_DENIED，等它完成后源已不存在）
 * - INVALID_NAME：名字已经过可移植性检查，出现时如果父目录已不存在，也是并发删除造成的
 */
static Error errorForPath(DWORD code, std::string_view utf8Path, bool mustExist = false)
{
	if (code == ERROR_PATH_NOT_FOUND || code == ERROR_DIRECTORY)
		return classifyMissing(utf8Path);
	if (mustExist && (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION ||
					  code == ERROR_DELETE_PENDING || code == ERROR_INVALID_NAME))
	{
		if (vanished(utf8Path))
			return classifyMissing(utf8Path);
	}
	if (code == ERROR_INVALID_NAME)
	{
		std::string_view parent = parentOf(utf8Path);
		if (!parent.empty() && vanished(parent))
			return classifyMissing(utf8Path);
	}
	return errorFromWin32(code);
}

Error statPath(std::string_view utf8Path, Stat& out)
{
	std::wstring wpath = toWidePath(utf8Path);
	WIN32_FILE_ATTRIBUTE_DATA data;
	DWORD code = retryTransient([&]() -> unsigned long
	{
		return ::GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &data) ? ERROR_SUCCESS : ::GetLastError();
	});
	if (code != ERROR_SUCCESS)
		return errorForPath(code, utf8Path, true);
	out.type = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? FileType::DIR : FileType::FILE;
	out.size = out.type == FileType::FILE
				   ? static_cast<int64_t>((static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow)
				   : 0;
	out.mtime = fileTimeToUnix(data.ftLastWriteTime);
	return Error::OK;
}

// ---------------------------------------------------------------- 修改操作（Win32）

namespace
{

// 这些常量和结构在较老的 SDK / MinGW 头文件里可能没有，按文档的数值和布局自己定义
constexpr int kFileDispositionInfoEx = 21;           // FILE_INFO_BY_HANDLE_CLASS::FileDispositionInfoEx
constexpr int kFileRenameInfoEx = 22;                // FILE_INFO_BY_HANDLE_CLASS::FileRenameInfoEx
constexpr DWORD kDispositionDelete = 0x1;            // FILE_DISPOSITION_FLAG_DELETE
constexpr DWORD kDispositionPosix = 0x2;             // FILE_DISPOSITION_FLAG_POSIX_SEMANTICS
constexpr DWORD kRenameReplace = 0x1;                // FILE_RENAME_FLAG_REPLACE_IF_EXISTS
constexpr DWORD kRenamePosix = 0x2;                  // FILE_RENAME_FLAG_POSIX_SEMANTICS

struct DispositionInfoEx
{
	DWORD Flags;
};

// 与 FILE_RENAME_INFO（RS1 之后 ReplaceIfExists 与 Flags 是 union）布局一致
struct RenameInfoEx
{
	DWORD Flags;
	HANDLE RootDirectory;
	DWORD FileNameLength;
	WCHAR FileName[1];
};

// 系统 / 文件系统不支持 *InfoEx（Windows 10 1709 之前、FAT、部分网络文件系统）时返回的错误
bool notSupported(DWORD code)
{
	return code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION ||
		   code == ERROR_CALL_NOT_IMPLEMENTED;
}

HANDLE openForDelete(const std::wstring& wpath)
{
	// 只申请 DELETE 权限，三种共享全开；不跟随符号链接（删除 / 改名的是链接本身）
	return ::CreateFileW(wpath.c_str(), DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
						 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

// POSIX 语义删除：名字立即移除，其他已打开的句柄继续有效。不支持时返回 notSupported 类错误
DWORD posixDelete(const std::wstring& wpath)
{
	HANDLE h = openForDelete(wpath);
	if (h == INVALID_HANDLE_VALUE)
		return ::GetLastError();
	DispositionInfoEx info{kDispositionDelete | kDispositionPosix};
	DWORD code = ::SetFileInformationByHandle(h, static_cast<FILE_INFO_BY_HANDLE_CLASS>(kFileDispositionInfoEx), &info,
											  sizeof(info))
					 ? ERROR_SUCCESS
					 : ::GetLastError();
	::CloseHandle(h);
	return code;
}

// POSIX 语义改名并替换已有目标：目标即使被其他句柄打开（带 FILE_SHARE_DELETE）也能替换
DWORD posixRename(const std::wstring& wfrom, const std::wstring& wto)
{
	HANDLE h = openForDelete(wfrom);
	if (h == INVALID_HANDLE_VALUE)
		return ::GetLastError();
	const size_t nameBytes = wto.size() * sizeof(WCHAR);
	std::vector<unsigned char> buf(sizeof(RenameInfoEx) + nameBytes);
	auto* info = reinterpret_cast<RenameInfoEx*>(buf.data());
	info->Flags = kRenameReplace | kRenamePosix;
	info->RootDirectory = nullptr;
	info->FileNameLength = static_cast<DWORD>(nameBytes);
	std::memcpy(info->FileName, wto.c_str(), nameBytes + sizeof(WCHAR));
	DWORD code = ::SetFileInformationByHandle(h, static_cast<FILE_INFO_BY_HANDLE_CLASS>(kFileRenameInfoEx), info,
											  static_cast<DWORD>(buf.size()))
					 ? ERROR_SUCCESS
					 : ::GetLastError();
	::CloseHandle(h);
	return code;
}

bool dirIsEmpty(const std::wstring& wdir, DWORD& code)
{
	WIN32_FIND_DATAW fd;
	HANDLE h = ::FindFirstFileExW((wdir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
	if (h == INVALID_HANDLE_VALUE)
	{
		code = ::GetLastError();
		return false;
	}
	bool empty = true;
	do
	{
		if (std::wcscmp(fd.cFileName, L".") != 0 && std::wcscmp(fd.cFileName, L"..") != 0)
		{
			empty = false;
			break;
		}
	} while (::FindNextFileW(h, &fd));
	::FindClose(h);
	code = ERROR_SUCCESS;
	return empty;
}

} // namespace

Error realName(std::string_view dir, std::string_view name, std::string& actual)
{
	std::string full(dir);
	full.push_back('/');
	full.append(name);
	WIN32_FIND_DATAW fd;
	HANDLE h = ::FindFirstFileExW(toWidePath(full).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
	if (h == INVALID_HANDLE_VALUE)
		return errorForPath(::GetLastError(), full, true);
	::FindClose(h);
	// cFileName 是长文件名；传入 8.3 短名时这里返回长名，按字节比较时自然判为不一致
	int len = ::WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nullptr, 0, nullptr, nullptr);
	actual.assign(len > 0 ? static_cast<size_t>(len - 1) : 0, '\0');
	if (len > 1)
		::WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, actual.data(), len, nullptr, nullptr);
	return Error::OK;
}

Error listDir(std::string_view path, const std::function<bool(const std::string& name, FileType type)>& cb)
{
	Stat st;
	Error err = statPath(path, st);
	if (err != Error::OK)
		return err;
	if (st.type != FileType::DIR)
		return Error::NOT_DIR;
	std::wstring pattern = toWidePath(path);
	if (!pattern.empty() && pattern.back() != L'\\')
		pattern.push_back(L'\\');
	pattern.push_back(L'*');
	WIN32_FIND_DATAW fd;
	HANDLE h = INVALID_HANDLE_VALUE;
	DWORD code = retryTransient([&]() -> unsigned long
	{
		h = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
							   FIND_FIRST_EX_LARGE_FETCH);
		return h != INVALID_HANDLE_VALUE ? ERROR_SUCCESS : ::GetLastError();
	});
	if (h == INVALID_HANDLE_VALUE)
	{
		// 驱动器根目录没有 "." ".."，空的时候返回 FILE_NOT_FOUND
		if (code == ERROR_FILE_NOT_FOUND)
			return Error::OK;
		return errorForPath(code, path, true);
	}
	do
	{
		const wchar_t* n = fd.cFileName;
		if ((n[0] == L'.' && n[1] == 0) || (n[0] == L'.' && n[1] == L'.' && n[2] == 0))
			continue;
		const FileType type = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? FileType::DIR : FileType::FILE;
		int len = ::WideCharToMultiByte(CP_UTF8, 0, n, -1, nullptr, 0, nullptr, nullptr);
		std::string name(len > 0 ? static_cast<size_t>(len - 1) : 0, '\0');
		if (len > 1)
			::WideCharToMultiByte(CP_UTF8, 0, n, -1, name.data(), len, nullptr, nullptr);
		if (!cb(name, type))
			break;
	} while (::FindNextFileW(h, &fd));
	::FindClose(h);
	return Error::OK;
}

Error makeDir(std::string_view path)
{
	std::wstring w = toWidePath(path);
	// 名字处于“删除挂起”（其他线程刚删掉、仍有句柄未关）时 CreateDirectoryW 返回 ACCESS_DENIED，重试即可
	DWORD code = retryTransient([&]() -> unsigned long
	{
		return ::CreateDirectoryW(w.c_str(), nullptr) ? ERROR_SUCCESS : ::GetLastError();
	});
	if (code == ERROR_SUCCESS)
		return Error::OK;
	return errorForPath(code, path);
}

Error removeDirPath(std::string_view path)
{
	std::wstring w = toWidePath(path);
	DWORD code = retryTransient([&]() -> unsigned long
	{
		DWORD c = posixDelete(w);
		if (notSupported(c))
			c = ::RemoveDirectoryW(w.c_str()) ? ERROR_SUCCESS : ::GetLastError();
		return c;
	});
	if (code == ERROR_SUCCESS)
		return Error::OK;
	return errorForPath(code, path, true);
}

Error removeFilePath(std::string_view path)
{
	std::wstring w = toWidePath(path);
	DWORD code = retryTransient([&]() -> unsigned long
	{
		DWORD c = posixDelete(w);
		if (notSupported(c))
			c = ::DeleteFileW(w.c_str()) ? ERROR_SUCCESS : ::GetLastError();
		return c;
	});
	if (code == ERROR_SUCCESS)
		return Error::OK;
	return errorForPath(code, path, true);
}

Error renamePath(std::string_view from, std::string_view to)
{
	const std::wstring wfrom = toWidePath(from);
	const std::wstring wto = toWidePath(to);
	DWORD fa = ::GetFileAttributesW(wfrom.c_str());
	if (fa == INVALID_FILE_ATTRIBUTES)
		return errorForPath(::GetLastError(), from, true);
	const bool fromDir = (fa & FILE_ATTRIBUTE_DIRECTORY) != 0;

	// MoveFileExW 不能用目录替换目录，也不能用文件替换目录。按 POSIX rename(2) 的语义预先处理。
	// 只差大小写的同一个名字（Windows 上是同一个文件）不做这些检查，单独处理
	DWORD ta = ::GetFileAttributesW(wto.c_str());
	const bool sameName = ::CompareStringOrdinal(wfrom.c_str(), static_cast<int>(wfrom.size()), wto.c_str(),
												 static_cast<int>(wto.size()), TRUE) == CSTR_EQUAL;
	if (sameName)
	{
		// 只改大小写：某些实现（例如 Wine）把它当成改名到自身而什么也不做，借一个临时名字分两步完成
		if (wfrom == wto)
			return Error::OK;
		const std::wstring wtmp = wfrom + L".vfs-case-tmp";
		DWORD code = retryTransient([&]() -> unsigned long
		{
			return ::MoveFileExW(wfrom.c_str(), wtmp.c_str(), 0) ? ERROR_SUCCESS : ::GetLastError();
		});
		if (code != ERROR_SUCCESS)
			return errorForPath(code, from, true);
		code = retryTransient([&]() -> unsigned long
		{
			return ::MoveFileExW(wtmp.c_str(), wto.c_str(), 0) ? ERROR_SUCCESS : ::GetLastError();
		});
		if (code != ERROR_SUCCESS)
		{
			::MoveFileExW(wtmp.c_str(), wfrom.c_str(), 0);   // 尽力恢复原名
			return errorFromWin32(code);
		}
		return Error::OK;
	}
	if (ta != INVALID_FILE_ATTRIBUTES)
	{
		const bool toDir = (ta & FILE_ATTRIBUTE_DIRECTORY) != 0;
		if (fromDir && !toDir)
			return vanished(from) ? classifyMissing(from) : Error::NOT_DIR;
		if (!fromDir && toDir)
			return vanished(from) ? classifyMissing(from) : Error::IS_DIR;
		if (fromDir && toDir)
		{
			// 这些预检查不是原子的：其他线程可能同时移走了源或目标。出错时重新确认两者，
			// 源已不存在报告 NOT_FOUND（与 rename(2) 一致），目标已不存在则直接改名
			DWORD code = ERROR_SUCCESS;
			const bool empty = dirIsEmpty(wto, code);
			if (code != ERROR_SUCCESS)
			{
				if (vanished(from))
					return classifyMissing(from);
				if (!vanished(to))
					return errorFromWin32(code);
			}
			else if (!empty)
				return vanished(from) ? classifyMissing(from) : Error::NOT_EMPTY;
			else
			{
				// 目录覆盖空目录：先删除目标（不是原子的，但结果与 POSIX 一致）
				Error e = removeDirPath(to);
				if (e != Error::OK && e != Error::NOT_FOUND)
					return vanished(from) ? classifyMissing(from) : e;
			}
		}
	}

	DWORD code = retryTransient([&]() -> unsigned long
	{
		DWORD flags = fromDir ? 0 : MOVEFILE_REPLACE_EXISTING;
		if (::MoveFileExW(wfrom.c_str(), wto.c_str(), flags))
			return ERROR_SUCCESS;
		DWORD c = ::GetLastError();
		// 目标文件被其他句柄打开时，传统语义的替换会失败（ACCESS_DENIED）；改用 POSIX 语义替换
		if (c == ERROR_ACCESS_DENIED && !fromDir)
		{
			DWORD p = posixRename(wfrom, wto);
			if (!notSupported(p))
				c = p;
		}
		return c;
	});
	if (code == ERROR_SUCCESS)
		return Error::OK;
	if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS)
		return fromDir ? Error::NOT_EMPTY : Error::EXISTS;
	if (code == ERROR_PATH_NOT_FOUND)
		return vanished(from) ? classifyMissing(from) : classifyMissing(to);
	return errorForPath(code, from, true);
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

Error realName(std::string_view dir, std::string_view name, std::string& actual)
{
	std::string full(dir);
	full.push_back('/');
	full.append(name);
#if defined(__APPLE__)
	// getattrlist 返回目录项中存储的名字（不跟随最后一级符号链接）
	struct attrlist al;
	std::memset(&al, 0, sizeof(al));
	al.bitmapcount = ATTR_BIT_MAP_COUNT;
	al.commonattr = ATTR_CMN_NAME;
	struct NameBuffer
	{
		u_int32_t length;
		attrreference_t ref;
		char data[1024];
	} __attribute__((aligned(4), packed));
	NameBuffer buf;
	if (::getattrlist(full.c_str(), &al, &buf, sizeof(buf), FSOPT_NOFOLLOW) != 0)
		return errorFromErrno(errno);
	const char* p = reinterpret_cast<const char*>(&buf.ref) + buf.ref.attr_dataoffset;
	size_t len = buf.ref.attr_length;   // 包含结尾的 '\0'
	while (len > 0 && p[len - 1] == '\0')
		--len;
	actual.assign(p, len);
	return Error::OK;
#else
	// Linux 等平台的文件系统按字节匹配名字：能 stat 到就是同名
	struct stat st;
	if (::lstat(full.c_str(), &st) != 0)
		return errorFromErrno(errno);
	actual.assign(name);
	return Error::OK;
#endif
}

Error listDir(std::string_view path, const std::function<bool(const std::string& name, FileType type)>& cb)
{
	Stat st;
	Error err = statPath(path, st);
	if (err != Error::OK)
		return err;
	if (st.type != FileType::DIR)
		return Error::NOT_DIR;
	std::error_code ec;
	std::filesystem::directory_iterator it(toFsPath(path), ec);
	if (ec)
		return errorFromCode(ec);
	for (; it != std::filesystem::directory_iterator(); it.increment(ec))
	{
		if (ec)
			return errorFromCode(ec);
		std::error_code ec2;
		// is_directory/is_regular_file 会跟随 symlink；特殊文件（设备、管道）直接跳过
		FileType type;
		if (it->is_directory(ec2))
			type = FileType::DIR;
		else if (it->is_regular_file(ec2))
			type = FileType::FILE;
		else
			continue;
		if (!cb(fromFsPath(it->path().filename()), type))
			break;
	}
	return ec ? errorFromCode(ec) : Error::OK;
}

Error makeDir(std::string_view path)
{
	std::string p(path);
	if (::mkdir(p.c_str(), 0777) == 0)
		return Error::OK;
	return errorFromErrno(errno);
}

Error removeDirPath(std::string_view path)
{
	std::string p(path);
	if (::rmdir(p.c_str()) == 0)
		return Error::OK;
	// POSIX 允许用 EEXIST 表示目录非空
	const int e = errno;
	return e == EEXIST ? Error::NOT_EMPTY : errorFromErrno(e);
}

Error removeFilePath(std::string_view path)
{
	std::string p(path);
	if (::unlink(p.c_str()) == 0)
		return Error::OK;
	// Linux 对目录返回 EISDIR，macOS / BSD 返回 EPERM，这里统一成 IS_DIR
	const int e = errno;
	if (e == EPERM || e == EISDIR)
	{
		struct stat st;
		if (::lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
			return Error::IS_DIR;
	}
	return errorFromErrno(e);
}

Error renamePath(std::string_view from, std::string_view to)
{
	std::string f(from), t(to);
	if (::rename(f.c_str(), t.c_str()) == 0)
		return Error::OK;
	// 目录覆盖非空目录：Linux 返回 ENOTEMPTY，部分系统返回 EEXIST（POSIX 两者都允许），统一成 NOT_EMPTY
	const int e = errno;
	return e == EEXIST ? Error::NOT_EMPTY : errorFromErrno(e);
}

#endif

} // namespace native

NS_VFS_END
