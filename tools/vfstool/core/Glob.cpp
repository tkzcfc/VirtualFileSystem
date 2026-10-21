#include "Glob.h"

NS_VFS_BEGIN

namespace tool
{

namespace
{

// Match a character class "[...]". p points just after '['; on success consumed is the total length of the class (including ']')
bool matchClass(std::string_view p, char c, size_t& consumed, bool& valid)
{
	size_t i = 0;
	bool negate = false;
	if (i < p.size() && (p[i] == '!' || p[i] == '^'))
	{
		negate = true;
		++i;
	}
	bool hit = false;
	bool first = true;
	while (i < p.size() && (p[i] != ']' || first))
	{
		first = false;
		char lo = p[i];
		if (lo == '\\' && i + 1 < p.size())
			lo = p[++i];
		char hi = lo;
		if (i + 2 < p.size() && p[i + 1] == '-' && p[i + 2] != ']')
		{
			hi = p[i + 2];
			if (hi == '\\' && i + 3 < p.size())
			{
				hi = p[i + 3];
				++i;
			}
			i += 2;
		}
		if (static_cast<unsigned char>(c) >= static_cast<unsigned char>(lo) &&
			static_cast<unsigned char>(c) <= static_cast<unsigned char>(hi))
			hit = true;
		++i;
	}
	if (i >= p.size())
	{
		// No closing ']': treat '[' as an ordinary character
		valid = false;
		return false;
	}
	valid = true;
	consumed = i + 2;   // '[' + body + ']'
	return hit != negate;
}

bool matchImpl(std::string_view p, std::string_view s)
{
	while (!p.empty())
	{
		if (p.size() >= 2 && p[0] == '*' && p[1] == '*')
		{
			std::string_view rest = p.substr(2);
			if (!rest.empty() && rest[0] == '/')
			{
				// "**/": match zero or more complete directories
				rest = rest.substr(1);
				if (matchImpl(rest, s))
					return true;
				for (size_t i = 0; i < s.size(); ++i)
				{
					if (s[i] == '/' && matchImpl(rest, s.substr(i + 1)))
						return true;
				}
				return false;
			}
			// "**" anywhere else: match any characters, including '/'
			for (size_t i = 0; i <= s.size(); ++i)
			{
				if (matchImpl(rest, s.substr(i)))
					return true;
			}
			return false;
		}

		char c = p[0];
		if (c == '*')
		{
			// "*": match any number of non-'/' characters
			std::string_view rest = p.substr(1);
			for (size_t i = 0; i <= s.size(); ++i)
			{
				if (matchImpl(rest, s.substr(i)))
					return true;
				if (i < s.size() && s[i] == '/')
					break;
			}
			return false;
		}
		if (s.empty())
			return false;
		if (c == '?')
		{
			if (s[0] == '/')
				return false;
			p.remove_prefix(1);
			s.remove_prefix(1);
			continue;
		}
		if (c == '[')
		{
			size_t consumed = 0;
			bool valid = false;
			bool hit = matchClass(p.substr(1), s[0], consumed, valid);
			if (valid)
			{
				if (!hit || s[0] == '/')
					return false;
				p.remove_prefix(consumed);
				s.remove_prefix(1);
				continue;
			}
		}
		if (c == '\\' && p.size() >= 2)
		{
			p.remove_prefix(1);
			c = p[0];
		}
		if (c != s[0])
			return false;
		p.remove_prefix(1);
		s.remove_prefix(1);
	}
	return s.empty();
}

} // namespace

bool Glob::match(std::string_view pattern, std::string_view path)
{
	// A leading '/' anchors the pattern at the source-directory root, equivalent to matching a full path without the '/'
	bool anchored = false;
	if (!pattern.empty() && pattern[0] == '/')
	{
		pattern.remove_prefix(1);
		anchored = true;
	}
	// A trailing '/' is only a "directory" hint; drop it
	while (pattern.size() > 1 && pattern.back() == '/')
		pattern.remove_suffix(1);

	if (matchImpl(pattern, path))
		return true;
	// A pattern with no '/': match the name at any depth (for example "*.png" matches "a/b/c.png", ".git" matches "x/.git")
	if (!anchored && pattern.find('/') == std::string_view::npos)
	{
		size_t pos = path.rfind('/');
		if (pos != std::string_view::npos)
			return matchImpl(pattern, path.substr(pos + 1));
	}
	return false;
}

bool Glob::evaluate(const std::vector<Rule>& rules, std::string_view path, bool defaultValue, bool* matched)
{
	bool result = defaultValue;
	bool any = false;
	for (const auto& rule : rules)
	{
		if (match(rule.pattern, path))
		{
			result = rule.positive;
			any = true;
		}
	}
	if (matched)
		*matched = any;
	return result;
}

} // namespace tool

NS_VFS_END
