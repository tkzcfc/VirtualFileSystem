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
	// On Windows use UTF-16 directly, and apply the long-path prefix along the way
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
	// Strip the long-path prefix so what is exposed (logs, realPath, path comparison) is always an ordinary Win32 path; both separators must be handled
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
	// On Windows std::filesystem mostly reports Win32 error codes in system_category, and mapping those as Win32 is more accurate
	if (ec.category() == std::system_category())
		return errorFromWin32(static_cast<unsigned long>(ec.value()));
#endif
	// On every other platform std::filesystem maps errors to errc in generic_category, so they are handled uniformly as errno
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
		// The UTF-16 code-unit count cannot exceed the UTF-8 byte count: convert once, without measuring the length first (saves a scan on the hot path).
		// MB_ERR_INVALID_CHARS: invalid UTF-8 yields an empty path (the following system call fails) instead of being replaced with U+FFFD,
		// otherwise two different byte strings could land on the same native file
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
	// Already a Win32 namespace path (\\?\ or a device path \\.\); leave it alone
	if (wide.compare(0, 4, L"\\\\?\\") == 0 || wide.compare(0, 4, L"\\\\.\\") == 0)
		return wide;
	// Short paths stay as they are; at 248 (CreateDirectoryW's limit is MAX_PATH - 12) convert to a prefixed absolute path.
	// After the prefix is added Win32 no longer normalizes the path, so GetFullPathNameW must produce the absolute, canonical form first
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
	// ERROR_INVALID_NAME: names passed down by the VFS have already passed the portability check. When it still appears, it is a transient state during concurrent deletion
	// (visible on Wine while a directory is being deleted); a genuinely illegal name costs only about 63ms extra
	return code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION ||
		   code == ERROR_DELETE_PENDING || code == ERROR_INVALID_NAME;
}

void sleepMs(unsigned long ms)
{
	::Sleep(static_cast<DWORD>(ms));
}

static_assert(ERROR_SHARING_VIOLATION == 32 && ERROR_LOCK_VIOLATION == 33, "error codes hard-coded in retryTransient");

static int64_t fileTimeToUnix(const FILETIME& ft)
{
	// FILETIME is a count of 100ns intervals since 1601-01-01
	ULARGE_INTEGER v;
	v.LowPart = ft.dwLowDateTime;
	v.HighPart = ft.dwHighDateTime;
	return static_cast<int64_t>(v.QuadPart / 10000000ULL) - 11644473600LL;
}

// When a path does not exist, distinguish "a parent directory does not exist" (NOT_FOUND) from "some parent path is a file" (NOT_DIR).
// Windows returns ERROR_PATH_NOT_FOUND for both, so the only option is to check level by level (this happens only on a miss).
// - rootLen > 0 (the first rootLen characters are a known existing backend root): check downward from the root. The first missing ancestor -> NOT_FOUND,
//   the first ancestor that is a file -> NOT_DIR. In a stacked mount the upper directory usually mirrors only a few subdirectories, and a whole missing subtree is the most common miss,
//   so one call usually decides it (bottom-up would query every missing parent);
// - otherwise walk bottom-up and find the first ancestor that exists
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

// Whether the path (or its parent directory) is already gone. An object with a delete pending has logically been deleted and counts as gone
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
 * Map a Win32 error code to Error, and handle races:
 * - PATH_NOT_FOUND / DIRECTORY: distinguish NOT_FOUND from NOT_DIR
 * - When mustExist (the operation targets an existing object: delete, rename source, list, stat), if the object is gone after ACCESS_DENIED / SHARING_VIOLATION /
 *   DELETE_PENDING / INVALID_NAME, another thread or process deleted or moved it, so report NOT_FOUND
 *   (for example another thread is renaming the same directory, this rename gets ACCESS_DENIED, and when that finishes the source is gone)
 * - INVALID_NAME: the name already passed the portability check; if the parent directory is gone when it appears, concurrent deletion caused it too
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

// Per-thread record for checkParentDir: the parent directory most recently confirmed to exist (full path)
static thread_local std::string t_knownParent;

Error checkParentDir(std::string_view utf8Path, size_t rootLen)
{
	// Measured (Windows 11 / NTFS): querying a path whose intermediate directory is missing (ERROR_PATH_NOT_FOUND) is serialized inside the system,
	// so 8 threads are slower than one; querying a missing name under a directory that exists scales linearly.
	// In a stacked mount the upper layer (the native override directory) usually mirrors only a few subdirectories, and almost every miss is the first kind.
	// So confirm the parent exists first (one extra query that scales). When the parent is missing, classify top-down and do not touch the leaf path
	if (rootLen == 0 || rootLen >= utf8Path.size())
		return Error::OK;
	std::string_view parent = parentOf(utf8Path);
	if (parent.size() <= rootLen)
		return Error::OK;   // The parent is the backend root
	// Querying the directory itself races concurrent queries of names inside it (also measured: 8 threads get much slower),
	// so each thread remembers the parent most recently confirmed to exist and skips the pre-check on a hit. This is only a positive hint:
	// if the directory is deleted later, the leaf query gets PATH_NOT_FOUND, the result is still correct (it just takes the slow path), and the record is cleared
	if (parent == t_knownParent)
		return Error::OK;
	DWORD attr = ::GetFileAttributesW(toWidePath(parent).c_str());
	if (attr == INVALID_FILE_ATTRIBUTES)
	{
		DWORD c = ::GetLastError();
		if (c == ERROR_FILE_NOT_FOUND || c == ERROR_PATH_NOT_FOUND)
			return classifyMissing(utf8Path, rootLen);
		return Error::OK;   // Any other error is left for the real operation to report
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

// ---------------------------------------------------------------- mutating operations (Win32)

namespace
{

/**
 * RAII wrapper for a FindFirstFileExW handle: FindClose still runs if a callback throws or the function returns early
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

// These constants and structures may be missing from older SDK / MinGW headers, so they are defined here from the documented values and layout
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

// Layout matches FILE_RENAME_INFO (since RS1, ReplaceIfExists and Flags are a union)
struct RenameInfoEx
{
	DWORD Flags;
	HANDLE RootDirectory;
	DWORD FileNameLength;
	WCHAR FileName[1];
};

// Errors returned when the system / file system does not support *InfoEx (before Windows 10 1709, FAT, some network file systems)
bool notSupported(DWORD code)
{
	return code == ERROR_INVALID_PARAMETER || code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION ||
		   code == ERROR_CALL_NOT_IMPLEMENTED;
}

HANDLE openForDelete(const std::wstring& wpath)
{
	// Request only DELETE access, with all three share modes open; do not follow symlinks (delete / rename acts on the link itself)
	return ::CreateFileW(wpath.c_str(), DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
						 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

// POSIX-semantics delete: the name is removed immediately and other open handles stay valid. If unsupported, return a notSupported-class error
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

// POSIX-semantics rename that replaces an existing target: the target can be replaced even when another handle has it open (with FILE_SHARE_DELETE)
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

// Whether a directory is empty. If enumeration fails midway (permissions, a network drive dropping), code is the error and the function returns false: a directory that was not fully enumerated must not be treated as empty
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
	// cFileName is the long file name; when an 8.3 short name was passed, this returns the long name, so a byte comparison naturally treats them as different
	int len = ::WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, nullptr, 0, nullptr, nullptr);
	actual.assign(len > 0 ? static_cast<size_t>(len - 1) : 0, '\0');
	if (len > 1)
		::WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, actual.data(), len, nullptr, nullptr);
	return Error::OK;
}

Error listDir(std::string_view path, const std::function<bool(const std::string& name, FileType type)>& cb)
{
	// Do not stat up front: when the directory exists (the common case) that saves a GetFileAttributesExW, and the cause is checked only if FindFirstFileExW fails
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
		// The failure may be: it does not exist, it is a file (ERROR_DIRECTORY / PATH_NOT_FOUND), or an empty drive root (FILE_NOT_FOUND,
		// no "." ".."). Distinguish with stat; a successful stat means the object is still there
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
		// Devices, pipes, and other special objects are not exposed to the VFS (same as POSIX)
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
	// FindNextFileW failing is not necessarily the end of the enumeration: a permission / I/O error midway must be reported, not returned as an incomplete directory
	code = ::GetLastError();
	if (code != ERROR_NO_MORE_FILES)
		return errorForPath(code, path, true);
	return Error::OK;
}

Error makeDir(std::string_view path)
{
	std::wstring w = toWidePath(path);
	// When the name is delete-pending (another thread just deleted it and a handle is still open), CreateDirectoryW returns ACCESS_DENIED; retrying is enough
	DWORD code = retryTransient([&]() -> unsigned long
	{
		return ::CreateDirectoryW(w.c_str(), nullptr) ? ERROR_SUCCESS : ::GetLastError();
	});
	if (code == ERROR_SUCCESS)
		return Error::OK;
	// Same as POSIX (mount-rules.md section 6): on failure the parent directory has already been deleted concurrently, so report NOT_FOUND,
	// not the ACCESS_DENIED that is common while a delete is pending
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
	// renamePathOnce has to pre-check directory-over-directory and file-over-directory (MoveFileExW itself does not handle those like rename(2)),
	// and those checks observe the source and the target at different times. Concurrent renames can see "source and target both exist", a state
	// that cannot occur under an atomic rename (for example another thread just renamed the source to the target, and yet another renames it back).
	// Conflict results from the pre-check (NOT_EMPTY / NOT_DIR / IS_DIR) are run again to confirm. A real conflict is stable
	// and yields the same result every time; a misjudgment caused by a race disappears on retry. At most 4 times, and only on the error path
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
// A temporary name unique within the process and across processes in the same directory: base.<tag>-<pid>-<n>.tmp.
// A fixed suffix collides when the same path is renamed concurrently, or when a previous crash left a file of that name
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

// Convert a UTF-16 path back to UTF-8 (only to hand an internally generated temporary path to a function that takes UTF-8)
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
	// The pre-check itself can see a short-lived error from a concurrent operation, and retries a limited number of times as well
	DWORD fa = INVALID_FILE_ATTRIBUTES;
	DWORD faCode = retryTransient([&]() -> unsigned long
	{
		fa = ::GetFileAttributesW(wfrom.c_str());
		return fa != INVALID_FILE_ATTRIBUTES ? ERROR_SUCCESS : ::GetLastError();
	});
	if (fa == INVALID_FILE_ATTRIBUTES)
		return errorForPath(faCode, from, true);
	const bool fromDir = (fa & FILE_ATTRIBUTE_DIRECTORY) != 0;
	std::wstring removedTarget;   // When a directory replaces an empty directory, the destination directory temporarily renamed out of the way

	// MoveFileExW cannot replace a directory with a directory, or a directory with a file. Handle that up front to match POSIX rename(2).
	// The same name differing only in case (one file on Windows) skips these checks and is handled separately
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
		// Case-only rename: some implementations (Wine, for example) treat it as renaming onto itself and do nothing, so do it in two steps through a temporary name
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
			// Rename back to the original name, retrying transient errors the same way; if restoring also fails, the file stays under the temporary name and IO_ERROR tells the caller the state is inconsistent
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
			// These pre-checks are not atomic: another thread may have moved the source or the target at the same time. On error, re-check both.
			// If the source is gone, report NOT_FOUND (same as rename(2)); if the target is gone, rename directly
			DWORD code = ERROR_SUCCESS;
			bool empty = false;
			retryTransient([&]() -> unsigned long
			{
				empty = dirIsEmpty(wto, code);
				return code;
			});
			if (code != ERROR_SUCCESS)
			{
				// Cannot tell whether the target is empty: if the source is gone, report NOT_FOUND; otherwise let MoveFileExW give the authoritative result
				// (if the target is still there it returns ALREADY_EXISTS -> NOT_EMPTY; if the target is gone it renames directly)
				if (vanished(from))
					return classifyMissing(from);
			}
			else if (!empty)
				return vanished(from) ? classifyMissing(from) : Error::NOT_EMPTY;
			else
			{
				// Directory over an empty directory: MoveFileExW cannot replace a directory. First rename the empty target to a temporary name to free the slot,
				// then delete it after the rename succeeds; if the rename fails, rename it back, so a failed rename still leaves the destination directory (not atomic, but the directory is not lost)
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
		// When the target file is open in another handle, a traditional replace fails (ACCESS_DENIED); use a POSIX-semantics replace instead
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
			// The old target moved out of the way is an empty directory. A failed delete (another process holding it, for example) only leaves an empty temporary directory and does not affect the rename result
			removeDirPath(fromWidePath(removedTarget));
		}
		else
		{
			// Rename failed: rename the destination directory back to its original name
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
		// After the source handle was opened, another thread renamed it to the target name (the handle follows the object), so renaming onto the target becomes "target already exists";
		// under atomic-rename semantics the source was already gone when this rename happened
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
	// stat follows symlinks; whether following is allowed is checked separately by NativeBackend
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
		// Devices, pipes, sockets, and other special files are not exposed to the VFS
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
	// getattrlist returns the name stored in the directory entry (it does not follow the final symlink)
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
	size_t len = buf.ref.attr_length;   // Includes the trailing '\0'
	while (len > 0 && p[len - 1] == '\0')
		--len;
	actual.assign(p, len);
	if (actual != name)
	{
		// Confirm again when only case differs: the iOS simulator file system is case-sensitive by bytes, but getattrlist still
		// matches another name case-insensitively; lstat of the original name then fails, which means the requested name itself does not exist
		struct stat st;
		if (::lstat(full.c_str(), &st) != 0)
			return errorFromErrno(errno);
	}
	return Error::OK;
#else
	// On Linux and similar platforms the file system matches names by bytes: if stat succeeds, it is the same name
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
		// is_directory/is_regular_file follow symlinks; special files (devices, pipes) are skipped outright
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
	// While a parent directory or a same-named directory is being deleted by another thread, errno differs by system: ENOENT on Linux,
	// and on macOS an error code that maps to INVALID_PATH has been observed. Re-check this kind of unexpected error: if the parent is already gone, report NOT_FOUND,
	// otherwise (the race is over) retry, at most 3 times
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
	// POSIX allows EEXIST to mean the directory is not empty
	const int e = errno;
	return e == EEXIST ? Error::NOT_EMPTY : errorFromErrno(e);
}

Error removeFilePath(std::string_view path)
{
	std::string p(path);
	if (::unlink(p.c_str()) == 0)
		return Error::OK;
	// Linux returns EISDIR for a directory, macOS / BSD return EPERM; both become IS_DIR here
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
	// Directory over a non-empty directory: Linux returns ENOTEMPTY, some systems return EEXIST (POSIX allows both); both become NOT_EMPTY
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

// ---------------------------------------------------------------- positional reads

#if VFS_PLATFORM_WINDOWS

namespace
{

// One manual-reset event per thread, for overlapped ReadFile to wait on completion; closed when the thread exits
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
	// ReOpenFile targets the same file object and does not go through the path: renaming or replacing the archive after mount cannot make the read see a different file
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
	// ReadFile returns TRUE when it completes synchronously, and ERROR_IO_PENDING when asynchronous. Both cases take the result from GetOverlappedResult
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
	// It must still be the same file: if the archive is replaced after mount (rename over the top), the new handle would point at the new file and disagree with the index already loaded
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
	// The first time a thread uses this, it is assigned a shard in turn and stays there; shards do not share a kernel file object
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
