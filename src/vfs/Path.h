// This file: tools for normalizing and splitting virtual paths.
// A virtual path is absolute, '/' separated, UTF-8, and case-sensitive; the canonical form is "/a/b", and the root is "/".
#pragma once

#include "Common.h"
#include "Error.h"

NS_VFS_BEGIN

/**
 * Path utilities. All functions are stateless and static, and they are thread-safe.
 */
class VFS_DLL Path
{
public:

	/**
	 * Normalize a virtual path: turn '\\' into '/', collapse repeated '/', drop '.', and resolve '..' literally.
	 * A path with no leading '/' is treated as relative to the root ("a/b" is the same as "/a/b").
	 * A trailing '/' is only a hint that "this is a directory" and is removed.
	 * @param in  Input path (UTF-8)
	 * @param out Output canonical path, for example "/a/b"; the root directory is "/"
	 * @return OK; INVALID_PATH if `..` escapes the root or the path contains NUL or a control character
	 */
	static Error normalize(std::string_view in, std::string& out);

	/**
	 * Normalize to a relative path (no leading '/', empty string for the root). Used for archive entry paths.
	 */
	static Error normalizeRelative(std::string_view in, std::string& out);

	/**
	 * Whether rel is already a canonical relative path: non-empty, no leading or trailing '/', no empty segments, no '.' or '..', no control characters.
	 * Archive readers use this to validate untrusted entry paths.
	 */
	static bool isNormalizedRelative(std::string_view rel);

	/**
	 * Whether the canonical path is under mountPoint (including equality). Compared by path segment, so "/ab" is not under "/a".
	 * @param path       Canonical path
	 * @param mountPoint Canonical path
	 * @param rel        Output: the part of path relative to mountPoint, with no leading '/'; empty when they are equal
	 */
	static bool isUnder(std::string_view path, std::string_view mountPoint, std::string_view& rel);

	/**
	 * Whether ancestor is a strict ancestor of path (equality does not count). Both are canonical paths.
	 */
	static bool isStrictAncestor(std::string_view ancestor, std::string_view path);

	/**
	 * Parent directory. "/a/b" -> "/a", "/a" -> "/", "/" -> "/".
	 */
	static std::string_view parent(std::string_view path);

	/**
	 * Final name segment. "/a/b" -> "b", "/" -> "". Also applies to relative paths.
	 */
	static std::string_view fileName(std::string_view path);

	/**
	 * Join a canonical directory and a name: "/" + "a" -> "/a", "/x" + "a" -> "/x/a".
	 */
	static std::string join(std::string_view dir, std::string_view name);

	/**
	 * Join a relative directory and a name: "" + "a" -> "a", "x" + "a" -> "x/a".
	 */
	static std::string joinRelative(std::string_view relDir, std::string_view name);

	/**
	 * Parent of a relative path: "a/b" -> "a", "a" -> "".
	 */
	static std::string_view parentRelative(std::string_view rel);

	/**
	 * Extra path-segment checks for the native backend: reject ':', a trailing '.' or space, and Windows reserved names (CON, NUL, COM1, and so on).
	 * Applied on every platform so resources behave the same everywhere.
	 */
	static bool isPortableComponent(std::string_view name);
};

NS_VFS_END
