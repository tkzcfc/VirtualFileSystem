// 本文件：本机文件系统相关的内部工具（UTF-8 路径转换、错误码映射、stat）。
// 不属于公开 API，只在库内部使用。
#pragma once

#include "../Common.h"
#include "../Error.h"
#include "../Stat.h"
#include <filesystem>
#include <system_error>

NS_VFS_BEGIN

namespace native
{

/**
 * UTF-8 字符串转成 std::filesystem::path。
 * 一律通过 char8_t 构造，保证 Windows 上非 ASCII 路径正确（决策 #28，禁止窄字符构造）。
 */
std::filesystem::path toFsPath(std::string_view utf8);

/**
 * std::filesystem::path 转回 UTF-8 字符串。
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
 * @return OK / NOT_FOUND / NOT_DIR（中间某一级是文件）/ 其他 IO 错误
 */
Error statPath(std::string_view utf8Path, Stat& out);

#if defined(_WIN32)
/**
 * UTF-8 转 UTF-16；绝对路径超过 MAX_PATH 时自动加 "\\\\?\\" 前缀（决策 #28）。
 */
std::wstring toWidePath(std::string_view utf8);
#endif

} // namespace native

NS_VFS_END
