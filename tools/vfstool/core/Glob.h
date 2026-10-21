// This file: glob matching used by pack rules.
// Syntax: `*` does not cross '/', `**` may cross (`**/` matches zero or more directories), `?` matches a single non-'/' character,
// `[abc]`, `[a-z]`, and `[!abc]` are character classes, and `\` escapes. A pattern that contains no '/' matches a name at any depth (same as gitignore).
#pragma once

#include "vfs/Common.h"

NS_VFS_BEGIN

namespace tool
{

/**
 * One rule: a pattern plus a positive/negative sense (include/exclude, encrypt/no-encrypt, compress/no-compress).
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
	 * Match.
	 * @param pattern glob pattern
	 * @param path    '/' path relative to the source directory, for example "a/b/c.png"
	 */
	static bool match(std::string_view pattern, std::string_view path);

	/**
	 * Apply rules in order; the last matching rule wins (similar to gitignore).
	 * @param rules        Rule list
	 * @param path         Relative path
	 * @param defaultValue Result when no rule matches
	 * @param matched      Optional: set to whether any rule matched
	 */
	static bool evaluate(const std::vector<Rule>& rules, std::string_view path, bool defaultValue,
						 bool* matched = nullptr);
};

} // namespace tool

NS_VFS_END
