// 本文件：虚拟路径的规范化与拆分工具。
// 虚拟路径：绝对路径、'/' 分隔、UTF-8、大小写敏感；规范形式为 "/a/b"，根为 "/"。
#pragma once

#include "Common.h"
#include "Error.h"

NS_VFS_BEGIN

/**
 * 路径工具，全部是无状态的静态函数，线程安全。
 */
class VFS_DLL Path
{
public:

	/**
	 * 规范化虚拟路径：'\\' 转成 '/'，合并重复的 '/'，去掉 '.'，按字面消解 '..'。
	 * 没有前导 '/' 的路径按相对根目录处理（"a/b" 等同于 "/a/b"）。
	 * 结尾的 '/' 只是“这是目录”的提示，会被去掉。
	 * @param in  输入路径（UTF-8）
	 * @param out 输出规范路径，例如 "/a/b"；根目录为 "/"
	 * @return OK；`..` 越过根、含 NUL 或控制字符时返回 INVALID_PATH
	 */
	static Error normalize(std::string_view in, std::string& out);

	/**
	 * 规范化成相对路径（无前导 '/'，根目录为空串）。用于归档条目路径。
	 */
	static Error normalizeRelative(std::string_view in, std::string& out);

	/**
	 * 判断 rel 是否已经是规范的相对路径：非空、无前导/结尾 '/'、无空段、无 '.' 和 '..'、无控制字符。
	 * 归档读取方用它校验不可信的条目路径。
	 */
	static bool isNormalizedRelative(std::string_view rel);

	/**
	 * 判断规范路径 path 是否位于挂载点 mountPoint 之下（含相等）。按路径段比较，"/ab" 不在 "/a" 之下。
	 * @param path       规范路径
	 * @param mountPoint 规范路径
	 * @param rel        输出：path 相对 mountPoint 的部分，不带前导 '/'；相等时为空串
	 */
	static bool isUnder(std::string_view path, std::string_view mountPoint, std::string_view& rel);

	/**
	 * 判断 ancestor 是否是 path 的真祖先（不含相等）。两者都是规范路径。
	 */
	static bool isStrictAncestor(std::string_view ancestor, std::string_view path);

	/**
	 * 父目录。"/a/b" -> "/a"，"/a" -> "/"，"/" -> "/"。
	 */
	static std::string_view parent(std::string_view path);

	/**
	 * 最后一段名字。"/a/b" -> "b"，"/" -> ""。对相对路径同样适用。
	 */
	static std::string_view fileName(std::string_view path);

	/**
	 * 拼接规范目录和名字："/" + "a" -> "/a"，"/x" + "a" -> "/x/a"。
	 */
	static std::string join(std::string_view dir, std::string_view name);

	/**
	 * 拼接相对目录和名字："" + "a" -> "a"，"x" + "a" -> "x/a"。
	 */
	static std::string joinRelative(std::string_view relDir, std::string_view name);

	/**
	 * 相对路径的父目录："a/b" -> "a"，"a" -> ""。
	 */
	static std::string_view parentRelative(std::string_view rel);

	/**
	 * 本机后端额外的路径段检查：拒绝 ':'、尾部的 '.' 或空格、Windows 保留名（CON、NUL、COM1 等）。
	 * 在所有平台上都执行，保证资源在各平台行为一致。
	 */
	static bool isPortableComponent(std::string_view name);
};

NS_VFS_END
