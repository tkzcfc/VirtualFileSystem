#include "Glob.h"

NS_VFS_BEGIN

namespace tool
{

namespace
{

// 匹配一个字符类 "[...]"。p 指向 '[' 之后；成功时 consumed 返回类的总长度（含 ']'）
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
		// 没有闭合的 ']'：把 '[' 当作普通字符
		valid = false;
		return false;
	}
	valid = true;
	consumed = i + 2;   // '[' + 内容 + ']'
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
				// "**/"：匹配零个或多个完整目录
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
			// 其他位置的 "**"：匹配任意字符，包括 '/'
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
			// "*"：匹配任意个非 '/' 字符
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
	// 前导 '/' 表示锚定在源目录根部，与不带 '/' 的完整路径匹配等价
	bool anchored = false;
	if (!pattern.empty() && pattern[0] == '/')
	{
		pattern.remove_prefix(1);
		anchored = true;
	}
	// 结尾的 '/' 只是“目录”提示，去掉
	while (pattern.size() > 1 && pattern.back() == '/')
		pattern.remove_suffix(1);

	if (matchImpl(pattern, path))
		return true;
	// 不含 '/' 的模式：匹配任意层级的名字（例如 "*.png" 匹配 "a/b/c.png"，".git" 匹配 "x/.git"）
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
