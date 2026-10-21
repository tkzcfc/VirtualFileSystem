// 本文件：VFS 层的错误码。
// 流层沿用 axmol 的 “-1 + errno” 风格；VFS 层的 API 统一返回 vfs::Error。
#pragma once

#include "Common.h"
#include <cerrno>

NS_VFS_BEGIN

/**
 * VFS 错误码。不使用异常。
 * 每个错误码都能通过 toErrno() 映射成最接近的 POSIX errno。
 */
enum class Error
{
	OK,                 // 成功
	NOT_FOUND,          // ENOENT：文件或目录不存在
	EXISTS,             // EEXIST：已经存在
	NOT_DIR,            // ENOTDIR：路径中间某一级不是目录，或者对文件做目录操作
	IS_DIR,             // EISDIR：对目录做文件操作
	NOT_EMPTY,          // ENOTEMPTY：目录非空
	READ_ONLY,          // EROFS：只读层、没有可写层，或写入后会被遮挡
	CROSS_DEVICE,       // EXDEV：跨后端 rename
	BUSY,               // EBUSY：挂载点不能删除/改名，或严格卸载时仍有打开的流
	INVALID_PATH,       // EINVAL：路径不合法（`..` 越过根、控制字符、Windows 保留名等）
	PERMISSION_DENIED,  // EACCES：操作系统拒绝，或 symlink 逃出根目录
	IO_ERROR,           // EIO：底层 IO 失败
	UNSUPPORTED,        // ENOTSUP：后端不支持该操作
	CORRUPTED,          // 归档损坏（magic、校验和、索引内容不合法）
	VERSION_MISMATCH,   // 归档版本与本程序不一致
	KEY_REQUIRED,       // 归档索引已加密，但既没有存储密钥也没有传入密钥
	KEY_INVALID,        // 传入的密钥错误，或者格式不对
};

/**
 * 把错误码转换成 POSIX errno 值（例如 Error::NOT_FOUND -> ENOENT）。
 */
VFS_DLL int toErrno(Error err);

/**
 * 返回错误码的英文名字（例如 "NOT_FOUND"），用于日志和 CLI 输出。
 */
VFS_DLL const char* errorName(Error err);

/**
 * 返回错误码的英文说明，用于 CLI 输出和日志。
 */
VFS_DLL const char* errorMessage(Error err);

// 小工具：把错误写入可选的输出参数；失败时同时设置 errno，方便与 axmol 风格兼容
inline void setError(Error* out, Error err)
{
	if (out)
		*out = err;
	if (err != Error::OK)
		errno = toErrno(err);
}

NS_VFS_END
