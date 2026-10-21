// 本文件：本机文件系统相关的内部工具（UTF-8 路径转换、错误码映射、stat 与 mkdir/rmdir/unlink/rename）。
// 不属于公开 API，只在库内部使用。
#pragma once

#include "../Common.h"
#include "../Error.h"
#include "../Stat.h"
#include <filesystem>
#include <functional>
#include <system_error>

NS_VFS_BEGIN

namespace native
{

/**
 * UTF-8 字符串转成 std::filesystem::path。
 * 一律通过 char8_t 构造，保证 Windows 上非 ASCII 路径正确。
 */
std::filesystem::path toFsPath(std::string_view utf8);

/**
 * std::filesystem::path 转回 UTF-8 字符串。Windows 上会去掉 toFsPath 可能加上的长路径前缀
 * （"\\\\?\\C:\\..." -> "C:\\..."，"\\\\?\\UNC\\server\\share" -> "\\\\server\\share"）。
 */
std::string fromFsPath(const std::filesystem::path& p);

/**
 * 把 std::error_code 映射成 vfs::Error。
 */
Error errorFromCode(const std::error_code& ec);

/**
 * 把 errno 值映射成 vfs::Error。
 */
Error errorFromErrno(int e);

/**
 * 查询本机路径的元数据。不跟随 symlink 的检查由调用方负责。
 * @param rootLen 可选：utf8Path 的前 rootLen 个字符是一个已知存在的目录（后端根目录）。
 *                Windows 上未命中时据此从根往下区分 NOT_FOUND / NOT_DIR，通常只需一次额外调用
 * @return OK / NOT_FOUND / NOT_DIR（中间某一级是文件）/ 其他 IO 错误
 */
Error statPath(std::string_view utf8Path, Stat& out, size_t rootLen = 0);

/**
 * 列目录：对每个子项调用 cb(name, type)，跳过 "." ".." 和特殊文件（设备、管道等）；cb 返回 false 停止。
 * 符号链接按它指向的类型报告。Windows 上直接用 FindFirstFileExW（支持长路径，不依赖各家 std::filesystem 的实现）。
 * @return OK / NOT_FOUND / NOT_DIR / 其他错误
 */
Error listDir(std::string_view path, const std::function<bool(const std::string& name, FileType type)>& cb);

/**
 * 当前线程最近一次本机操作失败时的原始错误码（POSIX 为 errno，Windows 为 GetLastError() 的值），
 * 在错误码映射时记录，只用于诊断（日志、测试输出）。没有失败过时为 0。
 */
int lastOsError();

/**
 * 查询目录 dir 中名为 name 的条目在磁盘上的真实名字（大小写不敏感的文件系统上可能与 name 只差大小写）。
 * Windows 用 FindFirstFileExW，macOS 用 getattrlist(ATTR_CMN_NAME)，都是一次系统调用；
 * 其他平台的文件系统按字节匹配，条目存在时 actual 就是 name。
 * @return OK / NOT_FOUND / 其他错误
 */
Error realName(std::string_view dir, std::string_view name, std::string& actual);

/**
 * 创建单级目录（mkdir）。已存在（目录或文件）返回 EXISTS；父目录不存在返回 NOT_FOUND，父路径是文件返回 NOT_DIR。
 */
Error makeDir(std::string_view path);

/**
 * 删除空目录（rmdir）。非空返回 NOT_EMPTY（POSIX 允许 rmdir 用 EEXIST 表示非空，这里统一成 NOT_EMPTY）。
 */
Error removeDirPath(std::string_view path);

/**
 * 删除文件（unlink）。Windows 上优先使用 POSIX 删除语义：名字立即消失，已打开的句柄继续可用。
 */
Error removeFilePath(std::string_view path);

/**
 * 改名（rename(2) 语义）：文件覆盖已有文件是原子的；目录可以覆盖空目录；
 * 目录覆盖非空目录返回 NOT_EMPTY，目录覆盖文件返回 NOT_DIR，文件覆盖目录返回 IS_DIR。
 * Windows 上目标被其他句柄打开（带 FILE_SHARE_DELETE）时用 POSIX 改名语义替换；
 * 目录覆盖空目录是“先删除目标再改名”，不是原子的。
 */
Error renamePath(std::string_view from, std::string_view to);

/**
 * 只读共享句柄，支持多个线程同时做位置读（不经过共享的文件指针）。
 * - POSIX：pread；
 * - Windows：以 FILE_FLAG_OVERLAPPED 打开，ReadFile 带 OVERLAPPED 偏移、每个线程一个等待事件。
 *   同步句柄上的并发 ReadFile 会在内核里按文件对象串行化，重叠句柄不会。
 * 归档来源用它代替“每次打开文件都新开一个 OS 句柄”：打开归档内的文件不再有系统调用。
 */
class PositionalReader
{
public:
	PositionalReader() = default;
	~PositionalReader();
	PositionalReader(const PositionalReader&) = delete;
	PositionalReader& operator=(const PositionalReader&) = delete;

	/**
	 * 打开文件（只读，允许其他句柄读写、删除、改名）。失败返回 false，errno 已设置。
	 */
	bool open(std::string_view utf8Path);

	bool isOpen() const;

	/**
	 * 从 offset 处读取最多 size 字节。线程安全。
	 * @return 实际读取的字节数（到达文件末尾时可能少于 size，在末尾或之后返回 0），出错返回 -1（errno 已设置）
	 */
	int64_t readAt(void* buf, size_t size, uint64_t offset) const;

private:
#if defined(_WIN32)
	void* m_handle = reinterpret_cast<void*>(static_cast<intptr_t>(-1));   // INVALID_HANDLE_VALUE
#else
	int m_fd = -1;
#endif
};

#if defined(_WIN32)
/**
 * UTF-8 转 UTF-16，'/' 换成 '\\'。路径长度达到 248（CreateDirectoryW 的上限 MAX_PATH - 12）时，
 * 先用 GetFullPathNameW 转成绝对路径，再加长路径前缀："\\\\?\\C:\\..."，UNC 路径为 "\\\\?\\UNC\\server\\share\\..."。
 * 已经带 "\\\\?\\" 或 "\\\\.\\" 前缀的路径原样返回。
 */
std::wstring toWidePath(std::string_view utf8);

/**
 * Win32 错误码映射成 vfs::Error。
 */
Error errorFromWin32(unsigned long code);

/**
 * 判断 Win32 错误是否可能是瞬时的（杀毒软件 / 索引服务短暂占用、并发删除留下的“删除挂起”状态）。
 */
bool isTransientWin32(unsigned long code);

/**
 * 对瞬时错误做有限次重试（总等待约 60ms）。fn 返回 Win32 错误码，0 表示成功。
 */
unsigned long retryTransient(const std::function<unsigned long()>& fn);
#endif

} // namespace native

NS_VFS_END
