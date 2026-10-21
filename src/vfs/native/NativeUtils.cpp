#include "NativeUtils.h"
#include <algorithm>
#include <atomic>

#if VFS_PLATFORM_WINDOWS
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
#    if VFS_PLATFORM_APPLE
#        include <sys/attr.h>
#        include <cstring>
#    endif
#    include <sys/stat.h>
#    include <fcntl.h>
#    include <unistd.h>
#    include <errno.h>
#    include <cstdio>
#endif

NS_VFS_BEGIN

namespace native
{

std::filesystem::path toFsPath(std::string_view utf8)
{
#if VFS_PLATFORM_WINDOWS
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
#if VFS_PLATFORM_WINDOWS
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

namespace
{
thread_local int t_lastOsError = 0;
}

int lastOsError()
{
	return t_lastOsError;
}

Error errorFromErrno(int e)
{
	if (e != 0)
		t_lastOsError = e;
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
#if VFS_PLATFORM_WINDOWS
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

#if VFS_PLATFORM_WINDOWS

std::wstring toWidePath(std::string_view utf8)
{
	std::wstring wide;
	if (!utf8.empty())
	{
		// UTF-16 码元数不会超过 UTF-8 字节数：一次转换，不先求长度（热路径上省一次扫描）。
		// MB_ERR_INVALID_CHARS：非法 UTF-8 得到空路径（随后的系统调用失败），而不是被替换成 U+FFFD，
		// 否则两个不同的字节串可能落到同一个本机文件上
		wide.resize(utf8.size());
		int len = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
										wide.data(), static_cast<int>(wide.size()));
		wide.resize(len > 0 ? static_cast<size_t>(len) : 0);
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
	if (code != 0)
		t_lastOsError = static_cast<int>(code);
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
	// ERROR_INVALID_NAME：VFS 传下来的名字都已通过可移植性检查，仍出现时是并发删除中的瞬时状态
	// （Wine 上目录正被删除时可见）；对真正非法的名字只多花约 63ms
	return code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION ||
		   code == ERROR_DELETE_PENDING || code == ERROR_INVALID_NAME;
}

void sleepMs(unsigned long ms)
{
	::Sleep(static_cast<DWORD>(ms));
}

static_assert(ERROR_SHARING_VIOLATION == 32 && ERROR_LOCK_VIOLATION == 33, "retryTransient 里写死的错误码");

static int64_t fileTimeToUnix(const FILETIME& ft)
{
	// FILETIME 是 1601-01-01 起的 100ns 计数
	ULARGE_INTEGER v;
	v.LowPart = ft.dwLowDateTime;
	v.HighPart = ft.dwHighDateTime;
	return static_cast<int64_t>(v.QuadPart / 10000000ULL) - 11644473600LL;
}

// 路径不存在时区分“父目录不存在”（NOT_FOUND）和“某一级父路径是文件”（NOT_DIR）。
// Windows 对两者都返回 ERROR_PATH_NOT_FOUND，只能逐级检查（只在未命中时发生）。
// - rootLen > 0（已知前 rootLen 个字符是存在的后端根目录）：从根往下检查，第一个不存在的祖先 → NOT_FOUND，
//   第一个是文件的祖先 → NOT_DIR。叠加挂载里上层目录只镜像了少量子目录，整棵子树都不存在是最常见的未命中，
//   这样通常一次调用就能确定（自底向上要把每一级不存在的父目录都查一遍）；
// - 否则自底向上，找到第一个存在的祖先
static Error classifyMissing(std::string_view utf8Path, size_t rootLen = 0)
{
	if (rootLen > 0 && rootLen < utf8Path.size())
	{
		size_t pos = rootLen;
		while (true)
		{
			size_t next = utf8Path.find_first_of("/\\", pos + 1);
			if (next == std::string_view::npos)
				break;
			DWORD attr = ::GetFileAttributesW(toWidePath(utf8Path.substr(0, next)).c_str());
			if (attr == INVALID_FILE_ATTRIBUTES)
				return Error::NOT_FOUND;
			if (!(attr & FILE_ATTRIBUTE_DIRECTORY))
				return Error::NOT_DIR;
			pos = next;
		}
		return Error::NOT_FOUND;
	}
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
static Error errorForPath(DWORD code, std::string_view utf8Path, bool mustExist = false, size_t rootLen = 0)
{
	t_lastOsError = static_cast<int>(code);
	if (code == ERROR_PATH_NOT_FOUND || code == ERROR_DIRECTORY)
		return classifyMissing(utf8Path, rootLen);
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

// checkParentDir 的每线程记录：最近确认存在的父目录（完整路径）
static thread_local std::string t_knownParent;

Error checkParentDir(std::string_view utf8Path, size_t rootLen)
{
	// 实测（Windows 11 / NTFS）：查询“中间目录不存在”的路径（ERROR_PATH_NOT_FOUND）在系统内部是串行的，
	// 8 线程反而比单线程慢；而查询“存在的目录下不存在的名字”可以线性扩展。
	// 叠加挂载里上层（本机覆盖目录）大多只镜像了少量子目录，未命中几乎都是前一种。
	// 所以先确认父目录存在（多一次可扩展的查询），父目录不存在时自顶向下分类，不去碰叶子路径
	if (rootLen == 0 || rootLen >= utf8Path.size())
		return Error::OK;
	std::string_view parent = parentOf(utf8Path);
	if (parent.size() <= rootLen)
		return Error::OK;   // 父目录就是后端根目录
	// 但查询目录本身会和对该目录内名字的并发查询互相竞争（同样实测 8 线程大幅变慢），
	// 所以每个线程记住最近确认存在的父目录，命中时跳过预检。这只是“正向”提示：
	// 目录之后被删掉，叶子查询会拿到 PATH_NOT_FOUND，结果依然正确（只是走回慢路径），并清掉记录
	if (parent == t_knownParent)
		return Error::OK;
	DWORD attr = ::GetFileAttributesW(toWidePath(parent).c_str());
	if (attr == INVALID_FILE_ATTRIBUTES)
	{
		DWORD c = ::GetLastError();
		if (c == ERROR_FILE_NOT_FOUND || c == ERROR_PATH_NOT_FOUND)
			return classifyMissing(utf8Path, rootLen);
		return Error::OK;   // 其他错误交给实际操作报告
	}
	if (!(attr & FILE_ATTRIBUTE_DIRECTORY))
		return Error::NOT_DIR;
	t_knownParent.assign(parent);
	return Error::OK;
}

void forgetParentDir()
{
	t_knownParent.clear();
}

Error statPath(std::string_view utf8Path, Stat& out, size_t rootLen)
{
	Error pe = checkParentDir(utf8Path, rootLen);
	if (pe != Error::OK)
		return pe;
	std::wstring wpath = toWidePath(utf8Path);
	WIN32_FILE_ATTRIBUTE_DATA data;
	DWORD code = retryTransient([&]() -> unsigned long
	{
		return ::GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &data) ? ERROR_SUCCESS : ::GetLastError();
	});
	if (code != ERROR_SUCCESS)
	{
		if (code == ERROR_PATH_NOT_FOUND || code == ERROR_DIRECTORY)
			forgetParentDir();
		return errorForPath(code, utf8Path, true, rootLen);
	}
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

/**
 * FindFirstFileExW 句柄的 RAII 包装：回调抛出异常、提前返回时也会 FindClose
 */
class FindHandle
{
public:
	explicit FindHandle(HANDLE h) : m_h(h) {}
	FindHandle(const FindHandle&) = delete;
	FindHandle& operator=(const FindHandle&) = delete;
	~FindHandle()
	{
		if (m_h != INVALID_HANDLE_VALUE)
			::FindClose(m_h);
	}
	HANDLE get() const { return m_h; }
	bool valid() const { return m_h != INVALID_HANDLE_VALUE; }

private:
	HANDLE m_h;
};

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

// 目录是否为空。枚举中途出错（权限、网络盘断开）时 code 为错误码、返回 false：不能把没枚举完的目录当成空目录
bool dirIsEmpty(const std::wstring& wdir, DWORD& code)
{
	WIN32_FIND_DATAW fd;
	FindHandle h(::FindFirstFileExW((wdir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0));
	if (!h.valid())
	{
		code = ::GetLastError();
		return false;
	}
	do
	{
		if (std::wcscmp(fd.cFileName, L".") != 0 && std::wcscmp(fd.cFileName, L"..") != 0)
		{
			code = ERROR_SUCCESS;
			return false;
		}
	} while (::FindNextFileW(h.get(), &fd));
	code = ::GetLastError();
	if (code == ERROR_NO_MORE_FILES)
	{
		code = ERROR_SUCCESS;
		return true;
	}
	return false;
}

} // namespace

Error realName(std::string_view dir, std::string_view name, std::string& actual)
{
	std::string full(dir);
	full.push_back('/');
	full.append(name);
	WIN32_FIND_DATAW fd;
	{
		FindHandle h(::FindFirstFileExW(toWidePath(full).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0));
		if (!h.valid())
			return errorForPath(::GetLastError(), full, true);
	}
	// cFileName 是长文件名；传入 8.3 短名时这里返回长名，按字节比较时自然判为不一致
	int len = ::WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nullptr, 0, nullptr, nullptr);
	actual.assign(len > 0 ? static_cast<size_t>(len - 1) : 0, '\0');
	if (len > 1)
		::WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, actual.data(), len, nullptr, nullptr);
	return Error::OK;
}

Error listDir(std::string_view path, const std::function<bool(const std::string& name, FileType type)>& cb)
{
	// 不预先 stat：目录存在时（常见情况）省掉一次 GetFileAttributesExW，只在 FindFirstFileExW 失败时再判断原因
	std::wstring pattern = toWidePath(path);
	if (!pattern.empty() && pattern.back() != L'\\')
		pattern.push_back(L'\\');
	pattern.push_back(L'*');
	WIN32_FIND_DATAW fd;
	HANDLE raw = INVALID_HANDLE_VALUE;
	DWORD code = retryTransient([&]() -> unsigned long
	{
		raw = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
								 FIND_FIRST_EX_LARGE_FETCH);
		return raw != INVALID_HANDLE_VALUE ? ERROR_SUCCESS : ::GetLastError();
	});
	FindHandle h(raw);
	if (!h.valid())
	{
		// 失败的原因可能是：不存在、是文件（ERROR_DIRECTORY / PATH_NOT_FOUND）、空的驱动器根目录（FILE_NOT_FOUND，
		// 没有 "." ".."）。用 stat 区分，stat 成功说明对象还在
		Stat st;
		Error err = statPath(path, st);
		if (err != Error::OK)
			return err;
		if (st.type != FileType::DIR)
			return Error::NOT_DIR;
		if (code == ERROR_FILE_NOT_FOUND)
			return Error::OK;
		return errorForPath(code, path, true);
	}
	do
	{
		const wchar_t* n = fd.cFileName;
		if ((n[0] == L'.' && n[1] == 0) || (n[0] == L'.' && n[1] == L'.' && n[2] == 0))
			continue;
		// 设备、管道等特殊对象不暴露给 VFS（与 POSIX 一致）
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DEVICE)
			continue;
		const FileType type = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? FileType::DIR : FileType::FILE;
		int len = ::WideCharToMultiByte(CP_UTF8, 0, n, -1, nullptr, 0, nullptr, nullptr);
		std::string name(len > 0 ? static_cast<size_t>(len - 1) : 0, '\0');
		if (len > 1)
			::WideCharToMultiByte(CP_UTF8, 0, n, -1, name.data(), len, nullptr, nullptr);
		if (!cb(name, type))
			return Error::OK;
	} while (::FindNextFileW(h.get(), &fd));
	// FindNextFileW 失败不一定是枚举结束：中途的权限 / I/O 错误要报告，不能返回一份不完整的目录
	code = ::GetLastError();
	if (code != ERROR_NO_MORE_FILES)
		return errorForPath(code, path, true);
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
	// 与 POSIX 一致（mount-rules.md §6）：失败时父目录已经被并发删除，报告 NOT_FOUND，
	// 而不是删除挂起状态下常见的 ACCESS_DENIED
	if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS)
	{
		std::string_view parent = parentOf(path);
		if (!parent.empty() && vanished(parent))
			return classifyMissing(path);
	}
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

namespace
{
Error renamePathOnce(std::string_view from, std::string_view to);
}

Error renamePath(std::string_view from, std::string_view to)
{
	// renamePathOnce 对目录覆盖、文件覆盖目录要先做几次检查（MoveFileExW 自己不按 rename(2) 处理这些），
	// 这些检查在不同时刻观察源和目标，并发改名时可能看到“源和目标同时存在”这种在原子 rename 下
	// 不可能出现的状态（例如另一线程刚把源改名成目标，再有线程改回来）。
	// 由预检查得出的冲突结果（NOT_EMPTY / NOT_DIR / IS_DIR）重新执行确认，真实的冲突是稳定的，
	// 每次都会得到同样结果；竞争造成的误判会在重试中消失。最多 4 次，只在出错路径上发生
	Error err = Error::OK;
	for (int attempt = 0; attempt < 4; ++attempt)
	{
		err = renamePathOnce(from, to);
		if (err != Error::NOT_EMPTY && err != Error::NOT_DIR && err != Error::IS_DIR)
			return err;
		if (attempt > 0)
			::Sleep(static_cast<DWORD>(attempt));
	}
	return err;
}

namespace
{
// 同一目录下进程内、进程间都唯一的临时名：base.<tag>-<pid>-<n>.tmp。
// 固定后缀在并发改名同一路径、或上次崩溃留下了同名文件时会冲突
std::wstring uniqueSiblingName(const std::wstring& base, const wchar_t* tag)
{
	static std::atomic<unsigned long long> s_counter{0};
	std::wstring name = base;
	name += L'.';
	name += tag;
	name += L'-';
	name += std::to_wstring(::GetCurrentProcessId());
	name += L'-';
	name += std::to_wstring(s_counter.fetch_add(1, std::memory_order_relaxed));
	name += L".tmp";
	return name;
}

// UTF-16 路径转回 UTF-8（只用于把内部生成的临时路径交给接受 UTF-8 的函数）
std::string fromWidePath(const std::wstring& w)
{
	int len = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
	std::string s(len > 0 ? static_cast<size_t>(len) : 0, '\0');
	if (len > 0)
		::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
	return s;
}

Error renamePathOnce(std::string_view from, std::string_view to)
{
	const std::wstring wfrom = toWidePath(from);
	const std::wstring wto = toWidePath(to);
	// 预检查本身也会遇到并发操作造成的短暂错误，同样有限重试
	DWORD fa = INVALID_FILE_ATTRIBUTES;
	DWORD faCode = retryTransient([&]() -> unsigned long
	{
		fa = ::GetFileAttributesW(wfrom.c_str());
		return fa != INVALID_FILE_ATTRIBUTES ? ERROR_SUCCESS : ::GetLastError();
	});
	if (fa == INVALID_FILE_ATTRIBUTES)
		return errorForPath(faCode, from, true);
	const bool fromDir = (fa & FILE_ATTRIBUTE_DIRECTORY) != 0;
	std::wstring removedTarget;   // 目录覆盖空目录时，被临时改名让出位置的目标目录

	// MoveFileExW 不能用目录替换目录，也不能用文件替换目录。按 POSIX rename(2) 的语义预先处理。
	// 只差大小写的同一个名字（Windows 上是同一个文件）不做这些检查，单独处理
	DWORD ta = INVALID_FILE_ATTRIBUTES;
	retryTransient([&]() -> unsigned long
	{
		ta = ::GetFileAttributesW(wto.c_str());
		return ta != INVALID_FILE_ATTRIBUTES ? ERROR_SUCCESS : ::GetLastError();
	});
	const bool sameName = ::CompareStringOrdinal(wfrom.c_str(), static_cast<int>(wfrom.size()), wto.c_str(),
												 static_cast<int>(wto.size()), TRUE) == CSTR_EQUAL;
	if (sameName)
	{
		// 只改大小写：某些实现（例如 Wine）把它当成改名到自身而什么也不做，借一个临时名字分两步完成
		if (wfrom == wto)
			return Error::OK;
		const std::wstring wtmp = uniqueSiblingName(wfrom, L"vfs-case");
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
			// 改回原名，同样对瞬时错误重试；恢复也失败时文件留在临时名下，返回 IO_ERROR 让调用方知道状态不一致
			DWORD restore = retryTransient([&]() -> unsigned long
			{
				return ::MoveFileExW(wtmp.c_str(), wfrom.c_str(), 0) ? ERROR_SUCCESS : ::GetLastError();
			});
			return restore == ERROR_SUCCESS ? errorFromWin32(code) : Error::IO_ERROR;
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
			bool empty = false;
			retryTransient([&]() -> unsigned long
			{
				empty = dirIsEmpty(wto, code);
				return code;
			});
			if (code != ERROR_SUCCESS)
			{
				// 无法确认目标是否为空：源已消失报告 NOT_FOUND；否则交给 MoveFileExW 给出权威结果
				// （目标仍存在时它返回 ALREADY_EXISTS → NOT_EMPTY，目标已消失时直接改名）
				if (vanished(from))
					return classifyMissing(from);
			}
			else if (!empty)
				return vanished(from) ? classifyMissing(from) : Error::NOT_EMPTY;
			else
			{
				// 目录覆盖空目录：MoveFileExW 不能替换目录。先把空的目标改到一个临时名让出位置，
				// 改名成功后再删掉它；改名失败时把它改回来，保证失败时目标目录仍在（不是原子的，但不会丢目录）
				removedTarget = uniqueSiblingName(wto, L"vfs-old");
				DWORD c = retryTransient([&]() -> unsigned long
				{
					return ::MoveFileExW(wto.c_str(), removedTarget.c_str(), 0) ? ERROR_SUCCESS : ::GetLastError();
				});
				if (c != ERROR_SUCCESS)
				{
					removedTarget.clear();
					if (c != ERROR_FILE_NOT_FOUND && c != ERROR_PATH_NOT_FOUND)
						return vanished(from) ? classifyMissing(from) : errorForPath(c, to, true);
				}
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
	if (!removedTarget.empty())
	{
		if (code == ERROR_SUCCESS)
		{
			// 让出位置的旧目标是空目录，删除失败（例如被其他进程占用）只会留下一个空的临时目录，不影响改名结果
			removeDirPath(fromWidePath(removedTarget));
		}
		else
		{
			// 改名失败：把目标目录改回原名
			retryTransient([&]() -> unsigned long
			{
				return ::MoveFileExW(removedTarget.c_str(), wto.c_str(), 0) ? ERROR_SUCCESS : ::GetLastError();
			});
		}
	}
	if (code == ERROR_SUCCESS)
		return Error::OK;
	if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS)
	{
		// 源句柄打开之后被其他线程改名成了目标名（句柄跟着对象走），再改名到目标就成了“目标已存在”；
		// 按原子 rename 的语义这次改名发生时源已不存在
		if (vanished(from))
			return classifyMissing(from);
		return fromDir ? Error::NOT_EMPTY : Error::EXISTS;
	}
	if (code == ERROR_PATH_NOT_FOUND)
		return vanished(from) ? classifyMissing(from) : classifyMissing(to);
	return errorForPath(code, from, true);
}

} // namespace

#else

Error checkParentDir(std::string_view /*utf8Path*/, size_t /*rootLen*/)
{
	return Error::OK;
}

void forgetParentDir() {}

Error statPath(std::string_view utf8Path, Stat& out, size_t /*rootLen*/)
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
#if VFS_PLATFORM_APPLE
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
	const size_t slash = p.find_last_of('/');
	const std::string parent = slash != std::string::npos && slash > 0 ? p.substr(0, slash) : std::string();
	Error err = Error::OK;
	// 父目录或同名目录正被其他线程删除时，不同系统给出的 errno 不一样：Linux 是 ENOENT，
	// macOS 上观察到映射成 INVALID_PATH 的错误码。对这类“意外”错误重新确认：父目录已经不存在按 NOT_FOUND
	// 报告，否则（竞争已经结束）重试，最多 3 次
	for (int attempt = 0; attempt < 3; ++attempt)
	{
		if (::mkdir(p.c_str(), 0777) == 0)
			return Error::OK;
		const int e = errno;
		err = errorFromErrno(e);
		if (err != Error::INVALID_PATH && err != Error::IO_ERROR)
			return err;
		struct stat st;
		if (!parent.empty() && ::stat(parent.c_str(), &st) != 0 && errno == ENOENT)
			return Error::NOT_FOUND;
		if (::stat(p.c_str(), &st) == 0)
			return Error::EXISTS;
	}
	return err;
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

uint64_t processId()
{
#if VFS_PLATFORM_WINDOWS
	return static_cast<uint64_t>(::GetCurrentProcessId());
#else
	return static_cast<uint64_t>(::getpid());
#endif
}

// ---------------------------------------------------------------- 位置读

#if VFS_PLATFORM_WINDOWS

namespace
{

// 每个线程一个手动复位事件，供重叠 ReadFile 等待完成；线程退出时关闭
struct ThreadEvent
{
	HANDLE handle = nullptr;
	~ThreadEvent()
	{
		if (handle)
			::CloseHandle(handle);
	}
	HANDLE get()
	{
		if (!handle)
			handle = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		return handle;
	}
};

thread_local ThreadEvent t_readEvent;

} // namespace

bool PositionalReader::open(std::string_view utf8Path)
{
	std::wstring wpath = toWidePath(utf8Path);
	HANDLE h = INVALID_HANDLE_VALUE;
	DWORD code = retryTransient([&]() -> unsigned long
	{
		h = ::CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
						  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
		return h != INVALID_HANDLE_VALUE ? ERROR_SUCCESS : ::GetLastError();
	});
	if (h == INVALID_HANDLE_VALUE)
	{
		t_lastOsError = static_cast<int>(code);
		errno = code == ERROR_PATH_NOT_FOUND ? ENOENT : toErrno(errorFromWin32(code));
		return false;
	}
	m_path.assign(utf8Path);
	m_handles[0].store(reinterpret_cast<intptr_t>(h), std::memory_order_release);
	return true;
}

intptr_t PositionalReader::reopen() const
{
	// ReOpenFile 针对同一个文件对象，不经过路径：归档在挂载后被改名、替换也不会读到别的文件
	HANDLE primary = reinterpret_cast<HANDLE>(m_handles[0].load(std::memory_order_acquire));
	HANDLE h = ::ReOpenFile(primary, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
							FILE_FLAG_OVERLAPPED);
	return h == INVALID_HANDLE_VALUE ? -1 : reinterpret_cast<intptr_t>(h);
}

void PositionalReader::closeHandle(intptr_t h)
{
	if (h != -1)
		::CloseHandle(reinterpret_cast<HANDLE>(h));
}

int64_t PositionalReader::readAt(void* buf, size_t size, uint64_t offset) const
{
	const intptr_t raw = handleForThread();
	if (raw == -1)
	{
		errno = EBADF;
		return -1;
	}
	HANDLE handle = reinterpret_cast<HANDLE>(raw);
	HANDLE ev = t_readEvent.get();
	if (!ev)
	{
		errno = ENOMEM;
		return -1;
	}
	const DWORD want = static_cast<DWORD>(std::min<size_t>(size, VFS_MAX_IO_CHUNK));
	OVERLAPPED ov;
	std::memset(&ov, 0, sizeof(ov));
	ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
	ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
	ov.hEvent = ev;
	DWORD got = 0;
	// 同步完成时 ReadFile 返回 TRUE；异步时返回 ERROR_IO_PENDING。两种情况都用 GetOverlappedResult 取结果
	if (!::ReadFile(handle, buf, want, nullptr, &ov))
	{
		const DWORD c = ::GetLastError();
		if (c != ERROR_IO_PENDING)
		{
			if (c == ERROR_HANDLE_EOF)
				return 0;
			t_lastOsError = static_cast<int>(c);
			errno = toErrno(errorFromWin32(c));
			return -1;
		}
	}
	if (!::GetOverlappedResult(handle, &ov, &got, TRUE))
	{
		DWORD c = ::GetLastError();
		if (c == ERROR_HANDLE_EOF)
			return 0;
		t_lastOsError = static_cast<int>(c);
		errno = toErrno(errorFromWin32(c));
		return -1;
	}
	return static_cast<int64_t>(got);
}

#else

bool PositionalReader::open(std::string_view utf8Path)
{
	std::string p(utf8Path);
	int flags = O_RDONLY;
#    if defined(O_CLOEXEC)
	flags |= O_CLOEXEC;
#    endif
	int fd = ::open(p.c_str(), flags);
	if (fd == -1)
		return false;
	struct stat st;
	if (::fstat(fd, &st) != 0 || S_ISDIR(st.st_mode))
	{
		const int e = S_ISDIR(st.st_mode) ? EISDIR : errno;
		::close(fd);
		errno = e;
		return false;
	}
	m_path = std::move(p);
	m_dev = static_cast<uint64_t>(st.st_dev);
	m_ino = static_cast<uint64_t>(st.st_ino);
	m_handles[0].store(fd, std::memory_order_release);
	return true;
}

intptr_t PositionalReader::reopen() const
{
	int flags = O_RDONLY;
#    if defined(O_CLOEXEC)
	flags |= O_CLOEXEC;
#    endif
	int fd = ::open(m_path.c_str(), flags);
	if (fd == -1)
		return -1;
	// 必须仍是同一个文件：挂载之后归档被替换（改名覆盖）时，新句柄会指向新文件，与已加载的索引不一致
	struct stat st;
	if (::fstat(fd, &st) != 0 || static_cast<uint64_t>(st.st_dev) != m_dev || static_cast<uint64_t>(st.st_ino) != m_ino)
	{
		::close(fd);
		return -1;
	}
	return fd;
}

void PositionalReader::closeHandle(intptr_t h)
{
	if (h != -1)
		::close(static_cast<int>(h));
}

int64_t PositionalReader::readAt(void* buf, size_t size, uint64_t offset) const
{
	const intptr_t fd = handleForThread();
	if (fd == -1)
	{
		errno = EBADF;
		return -1;
	}
	if (offset > static_cast<uint64_t>(INT64_MAX))
	{
		errno = EINVAL;
		return -1;
	}
	const size_t want = std::min<size_t>(size, VFS_MAX_IO_CHUNK);
	while (true)
	{
		ssize_t n = ::pread(static_cast<int>(fd), buf, want, static_cast<off_t>(offset));
		if (n >= 0)
			return static_cast<int64_t>(n);
		if (errno != EINTR)
			return -1;
	}
}

#endif

PositionalReader::PositionalReader()
{
	for (size_t i = 0; i < SLOTS; ++i)
	{
		m_handles[i].store(-1, std::memory_order_relaxed);
		m_failed[i].store(false, std::memory_order_relaxed);
	}
}

PositionalReader::~PositionalReader()
{
	for (size_t i = 0; i < SLOTS; ++i)
		closeHandle(m_handles[i].load(std::memory_order_relaxed));
}

bool PositionalReader::isOpen() const
{
	return m_handles[0].load(std::memory_order_acquire) != -1;
}

intptr_t PositionalReader::handleForThread() const
{
	// 线程第一次使用时轮流分配一个分片，之后固定；分片之间互不共享内核文件对象
	static std::atomic<size_t> s_next{0};
	thread_local const size_t t_slot = s_next.fetch_add(1, std::memory_order_relaxed);
	const size_t slot = t_slot % SLOTS;
	intptr_t h = m_handles[slot].load(std::memory_order_acquire);
	if (h != -1)
		return h;
	const intptr_t primary = m_handles[0].load(std::memory_order_acquire);
	if (slot == 0 || primary == -1 || m_failed[slot].load(std::memory_order_relaxed))
		return primary;
	std::lock_guard<std::mutex> lock(m_openMutex);
	h = m_handles[slot].load(std::memory_order_acquire);
	if (h != -1)
		return h;
	h = reopen();
	if (h == -1)
	{
		m_failed[slot].store(true, std::memory_order_relaxed);
		return primary;
	}
	m_handles[slot].store(h, std::memory_order_release);
	return h;
}

} // namespace native

NS_VFS_END
