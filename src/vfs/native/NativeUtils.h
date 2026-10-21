// This file: internal helpers for the native filesystem (UTF-8 path conversion, error-code mapping, stat, and mkdir/rmdir/unlink/rename).
// Not part of the public API; used only inside the library.
#pragma once

#include "../Common.h"
#include "../Error.h"
#include "../Stat.h"
#include <filesystem>
#include <string>
#include <mutex>
#include <atomic>
#include <functional>
#include <system_error>

NS_VFS_BEGIN

namespace native
{

/**
 * Convert a UTF-8 string to std::filesystem::path.
 * Always constructed via char8_t so non-ASCII paths are correct on Windows.
 */
std::filesystem::path toFsPath(std::string_view utf8);

/**
 * Convert std::filesystem::path back to a UTF-8 string. On Windows, strip the long-path prefix toFsPath may have added
 * ("\\\\?\\C:\\..." -> "C:\\...", "\\\\?\\UNC\\server\\share" -> "\\\\server\\share").
 */
std::string fromFsPath(const std::filesystem::path& p);

/**
 * Map a std::error_code to vfs::Error.
 */
Error errorFromCode(const std::error_code& ec);

/**
 * Map an errno value to vfs::Error.
 */
Error errorFromErrno(int e);

/**
 * Query metadata for a native path. The caller is responsible for any no-follow-symlink check.
 * @param rootLen optional: the first rootLen characters of utf8Path are a directory known to exist (the backend root).
 *                On Windows a miss uses that to walk down from the root and tell NOT_FOUND from NOT_DIR, usually with one extra call
 * @return OK / NOT_FOUND / NOT_DIR (some intermediate component is a file) / other I/O errors
 */
Error statPath(std::string_view utf8Path, Stat& out, size_t rootLen = 0);

/**
 * Parent-directory precheck before a read-only query (Windows; other platforms always return OK).
 * On Windows, querying a path whose intermediate directory does not exist is serialized inside the system and becomes a bottleneck when many threads miss,
 * so confirming the parent exists first avoids that. Missing parent -> NOT_FOUND, parent path is a file -> NOT_DIR, otherwise OK
 * @param rootLen the first rootLen characters of utf8Path are the backend root, known to exist; 0 skips the precheck
 */
Error checkParentDir(std::string_view utf8Path, size_t rootLen);

/// Forget the parent directory this thread's checkParentDir remembered (call when open fails and the parent may be gone)
void forgetParentDir();

/**
 * List a directory: call cb(name, type) for each child, skipping "." ".." and special files (devices, pipes, and so on); cb returning false stops.
 * Symlinks are reported as the type they point at. On Windows this uses FindFirstFileExW directly (long paths, independent of each std::filesystem implementation).
 * @return OK / NOT_FOUND / NOT_DIR / other errors
 */
Error listDir(std::string_view path, const std::function<bool(const std::string& name, FileType type)>& cb);

/**
 * Raw error code from the last native operation that failed on this thread (errno on POSIX, the GetLastError() value on Windows),
 * recorded while mapping the error code, for diagnostics only (logs, test output). 0 if nothing has failed.
 */
int lastOsError();

/**
 * Look up the on-disk name of the entry named name in directory dir (on a case-insensitive filesystem it may differ from name only in case).
 * Windows uses FindFirstFileExW, macOS uses getattrlist(ATTR_CMN_NAME); each is one system call.
 * On other platforms the filesystem matches bytes, so when the entry exists actual is name.
 * @return OK / NOT_FOUND / other errors
 */
Error realName(std::string_view dir, std::string_view name, std::string& actual);

/**
 * Create a single directory (mkdir). EXISTS if it already exists (directory or file); NOT_FOUND if the parent is missing, NOT_DIR if the parent path is a file.
 */
Error makeDir(std::string_view path);

/**
 * Remove an empty directory (rmdir). NOT_EMPTY if it is not empty (POSIX allows rmdir to report a non-empty directory as EEXIST; that is unified to NOT_EMPTY here).
 */
Error removeDirPath(std::string_view path);

/**
 * Delete a file (unlink). On Windows, prefer POSIX delete semantics: the name disappears immediately and already-open handles stay usable.
 */
Error removeFilePath(std::string_view path);

/**
 * Rename (rename(2) semantics): a file replacing an existing file is atomic; a directory may replace an empty directory;
 * a directory replacing a non-empty directory returns NOT_EMPTY, a directory replacing a file returns NOT_DIR, a file replacing a directory returns IS_DIR.
 * On Windows, if the target is open in another handle (with FILE_SHARE_DELETE), replace it with POSIX rename semantics;
 * a directory replacing an empty directory is "delete the target, then rename", and is not atomic.
 */
Error renamePath(std::string_view from, std::string_view to);

/**
 * Current process id (used to build temporary names unique across processes).
 */
uint64_t processId();

/**
 * A shared read-only handle. Several threads can read by offset at once (no shared file pointer).
 * - POSIX: pread;
 * - Windows: opened with FILE_FLAG_OVERLAPPED; ReadFile takes an OVERLAPPED offset and each thread has its own wait event.
 *   Concurrent ReadFile on a synchronous handle is serialized in the kernel per file object; an overlapped handle is not.
 * Archive sources use this instead of "a new OS handle every time a file is opened": opening a file inside the archive is no longer a system call.
 *
 * With one handle shared by every thread, each read updates the same file object's reference count in the kernel (fdget in a multithreaded Linux process),
 * and that one cache line becomes the bottleneck for 8 threads doing random reads. So split by thread into at most SLOTS handles: slot 0 is opened in open,
 * and the rest lazily reopen "the same file object" the first time a shard is used:
 * - POSIX: reopen by path, then check st_dev/st_ino. If it differs from handle 0 (the file was replaced after mount), give up and keep using handle 0;
 * - Windows: ReOpenFile on handle 0, without going through the path.
 * If the open fails (handle exhaustion and similar), fall back to handle 0 as well. Correctness is unchanged.
 */
class PositionalReader
{
public:
	static constexpr size_t SLOTS = 8;

	PositionalReader();
	~PositionalReader();
	PositionalReader(const PositionalReader&) = delete;
	PositionalReader& operator=(const PositionalReader&) = delete;

	/**
	 * Open the file (read-only; other handles may read, write, delete, and rename). Returns false on failure, with errno set.
	 */
	bool open(std::string_view utf8Path);

	bool isOpen() const;

	/**
	 * Read at most size bytes from offset. Thread-safe.
	 * @return bytes actually read (may be less than size at EOF, 0 at or past the end), or -1 on error (errno is set)
	 */
	int64_t readAt(void* buf, size_t size, uint64_t offset) const;

private:
	// Handle for the current thread (lazily opens this shard if needed)
	intptr_t handleForThread() const;

	// Open a new handle to the same file object for a shard; -1 on failure
	intptr_t reopen() const;

	static void closeHandle(intptr_t h);

	std::string m_path;
	mutable std::atomic<intptr_t> m_handles[SLOTS];   // fd or HANDLE; -1 means not opened yet; slot 0 is the primary handle
	mutable std::atomic<bool> m_failed[SLOTS];         // this shard failed to reopen; use the primary handle
	mutable std::mutex m_openMutex;                    // serializes lazy opens of shards
#if !VFS_PLATFORM_WINDOWS
	uint64_t m_dev = 0;
	uint64_t m_ino = 0;
#endif
};

#if VFS_PLATFORM_WINDOWS
/**
 * UTF-8 to UTF-16, '/' replaced with '\\'. When the path length reaches 248 (CreateDirectoryW's limit, MAX_PATH - 12),
 * turn it into an absolute path with GetFullPathNameW, then add the long-path prefix: "\\\\?\\C:\\...", or "\\\\?\\UNC\\server\\share\\..." for UNC.
 * A path that already has a "\\\\?\\" or "\\\\.\\" prefix is returned unchanged.
 */
std::wstring toWidePath(std::string_view utf8);

/**
 * Map a Win32 error code to vfs::Error.
 */
Error errorFromWin32(unsigned long code);

/**
 * Whether a Win32 error might be transient (antivirus / indexer briefly holding the file, or a "delete pending" state left by a concurrent delete).
 */
bool isTransientWin32(unsigned long code);

/**
 * Sleep the current thread for ms milliseconds (a thin wrapper around Sleep; the header does not include windows.h).
 */
void sleepMs(unsigned long ms);

/**
 * Retry a limited number of times on transient errors. fn returns a Win32 error code; 0 means success.
 * The first try does not wait; then 1, 2, 4, 8, 16, 32 ms, about 63 ms at most. A real permission error (ACCESS_DENIED) costs only that much extra.
 * SHARING_VIOLATION / LOCK_VIOLATION always come from another handle (antivirus, the indexer, or another thread in this process),
 * not a real permission problem, so wait three more times at 64 ms, about 255 ms in total.
 * Written as a template: the success path (success on the first try) is one inline call, with no std::function type erasure and no possible heap allocation.
 */
template <typename Fn>
unsigned long retryTransient(Fn&& fn)
{
	static constexpr unsigned long delays[] = {0, 1, 2, 4, 8, 16, 32, 64, 64, 64};
	constexpr size_t shortCount = 7;
	constexpr unsigned long sharingViolation = 32;   // ERROR_SHARING_VIOLATION
	constexpr unsigned long lockViolation = 33;      // ERROR_LOCK_VIOLATION
	unsigned long code = fn();
	if (code == 0 || !isTransientWin32(code))
		return code;
	for (size_t i = 1; i < sizeof(delays) / sizeof(delays[0]); ++i)
	{
		const bool sharing = code == sharingViolation || code == lockViolation;
		if (i >= shortCount && !sharing)
			break;
		sleepMs(delays[i]);
		code = fn();
		if (!isTransientWin32(code))
			break;
	}
	return code;
}
#endif

} // namespace native

NS_VFS_END
