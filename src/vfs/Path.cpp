#include "Path.h"

NS_VFS_BEGIN

Error Path::normalize(std::string_view in, std::string& out)
{
	// A stack records the start of each segment in out. On ".." the output is truncated directly, avoiding an extra vector<string> allocation
	std::string result;
	result.reserve(in.size() + 1);
	std::vector<size_t> segStarts;

	size_t i = 0;
	const size_t n = in.size();
	while (i < n)
	{
		// Skip separators ('/' and '\\' are equivalent)
		while (i < n && (in[i] == '/' || in[i] == '\\'))
			++i;
		if (i >= n)
			break;

		size_t start = i;
		while (i < n && in[i] != '/' && in[i] != '\\')
		{
			unsigned char c = static_cast<unsigned char>(in[i]);
			// Reject NUL, control characters, and DEL: their meaning differs across platform file systems, and they are often used for injection
			if (c < 0x20 || c == 0x7F)
				return Error::INVALID_PATH;
			++i;
		}
		std::string_view seg = in.substr(start, i - start);

		if (seg == ".")
			continue;
		if (seg == "..")
		{
			// Crossing the root is an error; do not truncate
			if (segStarts.empty())
				return Error::INVALID_PATH;
			result.resize(segStarts.back());
			segStarts.pop_back();
			continue;
		}
		segStarts.push_back(result.size());
		result.push_back('/');
		result.append(seg);
	}

	if (result.empty())
		result = "/";
	out = std::move(result);
	return Error::OK;
}

Error Path::normalizeRelative(std::string_view in, std::string& out)
{
	std::string tmp;
	Error err = normalize(in, tmp);
	if (err != Error::OK)
		return err;
	out.assign(tmp, 1, std::string::npos);
	return Error::OK;
}

bool Path::isNormalizedRelative(std::string_view rel)
{
	if (rel.empty() || rel.front() == '/' || rel.back() == '/')
		return false;

	size_t start = 0;
	while (start <= rel.size())
	{
		size_t end = rel.find('/', start);
		if (end == std::string_view::npos)
			end = rel.size();
		std::string_view seg = rel.substr(start, end - start);
		if (seg.empty() || seg == "." || seg == "..")
			return false;
		for (char ch : seg)
		{
			unsigned char c = static_cast<unsigned char>(ch);
			if (c < 0x20 || c == 0x7F || c == '\\')
				return false;
		}
		start = end + 1;
	}
	return true;
}

bool Path::isUnder(std::string_view path, std::string_view mountPoint, std::string_view& rel)
{
	// A root mount point covers every path
	if (mountPoint.size() == 1)
	{
		rel = path.substr(1);
		return true;
	}
	if (path.size() < mountPoint.size() || path.compare(0, mountPoint.size(), mountPoint) != 0)
		return false;
	if (path.size() == mountPoint.size())
	{
		rel = std::string_view();
		return true;
	}
	// Must fall on a segment boundary: "/ab" is not under "/a"
	if (path[mountPoint.size()] != '/')
		return false;
	rel = path.substr(mountPoint.size() + 1);
	return true;
}

bool Path::isStrictAncestor(std::string_view ancestor, std::string_view path)
{
	if (ancestor.size() >= path.size())
		return false;
	std::string_view rel;
	return isUnder(path, ancestor, rel);
}

std::string_view Path::parent(std::string_view path)
{
	size_t pos = path.rfind('/');
	if (pos == std::string_view::npos || pos == 0)
		return std::string_view("/");
	return path.substr(0, pos);
}

std::string_view Path::fileName(std::string_view path)
{
	size_t pos = path.rfind('/');
	if (pos == std::string_view::npos)
		return path;
	return path.substr(pos + 1);
}

std::string Path::join(std::string_view dir, std::string_view name)
{
	std::string result;
	result.reserve(dir.size() + name.size() + 1);
	result.append(dir);
	if (result.empty() || result.back() != '/')
		result.push_back('/');
	result.append(name);
	return result;
}

std::string Path::joinRelative(std::string_view relDir, std::string_view name)
{
	if (relDir.empty())
		return std::string(name);
	std::string result;
	result.reserve(relDir.size() + name.size() + 1);
	result.append(relDir);
	result.push_back('/');
	result.append(name);
	return result;
}

std::string_view Path::parentRelative(std::string_view rel)
{
	size_t pos = rel.rfind('/');
	if (pos == std::string_view::npos)
		return std::string_view();
	return rel.substr(0, pos);
}

bool Path::isPortableComponent(std::string_view name)
{
	if (name.empty())
		return false;
	// ':' is the drive-letter / alternate-data-stream separator on Windows (PhysicsFS forbids it too)
	if (name.find(':') != std::string_view::npos)
		return false;
	// Windows silently strips a trailing '.' or space, so "a." and "a" would refer to the same file
	if (name.back() == '.' && name != "." && name != "..")
		return false;
	if (name.back() == ' ')
		return false;

	// Windows reserved device names, case-insensitive; the reservation also applies with an extension (for example "nul.txt")
	std::string_view stem = name.substr(0, name.find('.'));
	auto upperEq = [](std::string_view a, const char* b)
	{
		size_t i = 0;
		for (; i < a.size() && b[i]; ++i)
		{
			char c = a[i];
			if (c >= 'a' && c <= 'z')
				c = static_cast<char>(c - 'a' + 'A');
			if (c != b[i])
				return false;
		}
		return i == a.size() && b[i] == '\0';
	};
	static const char* const reserved[] = {"CON", "PRN", "AUX", "NUL"};
	for (const char* r : reserved)
	{
		if (upperEq(stem, r))
			return false;
	}
	// COM1..COM9, LPT1..LPT9
	if (stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9')
	{
		std::string_view head = stem.substr(0, 3);
		if (upperEq(head, "COM") || upperEq(head, "LPT"))
			return false;
	}
	return true;
}

NS_VFS_END
