// 本文件：打包规则使用的 glob 匹配。
// 语法：`*` 不跨越 '/'，`**` 可以跨越（`**/` 匹配零个或多个目录），`?` 匹配单个非 '/' 字符，
// `[abc]`、`[a-z]`、`[!abc]` 字符类，`\` 转义。不含 '/' 的模式匹配任意层级的名字（与 gitignore 一致）。
#pragma once

#include "vfs/Common.h"

NS_VFS_BEGIN

namespace tool
{

/**
 * 一条规则：模式 + 正/反（include/exclude、encrypt/no-encrypt、compress/no-compress）。
 */
struct Rule
{
	std::string pattern;
	bool positive = true;
};

class Glob
{
public:

	/**
	 * 匹配。
	 * @param pattern glob 模式
	 * @param path    相对源目录的 '/' 路径，例如 "a/b/c.png"
	 */
	static bool match(std::string_view pattern, std::string_view path);

	/**
	 * 按顺序应用规则，最后一条匹配的规则生效（类似 gitignore）。
	 * @param rules        规则列表
	 * @param path         相对路径
	 * @param defaultValue 没有任何规则匹配时的结果
	 * @param matched      可选：输出是否有规则匹配
	 */
	static bool evaluate(const std::vector<Rule>& rules, std::string_view path, bool defaultValue,
						 bool* matched = nullptr);
};

} // namespace tool

NS_VFS_END
