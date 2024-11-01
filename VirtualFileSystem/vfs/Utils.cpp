#include "Utils.h"
#include <algorithm>

NS_VFS_BEGIN

std::string convertPathFormatToUnixStyle(const std::string& path)
{
	std::string ret{ path };
	std::replace(ret.begin(), ret.end(), '\\', '/');
	return ret;
}

void formatDirPath(std::string& path)
{
	if (!path.empty() && path.back() != '/')
		path.push_back('/');
}

std::string convertDirPath(const std::string& path)
{
	auto outPath = convertPathFormatToUnixStyle(path);
	formatDirPath(outPath);
	return outPath;
}

std::string getFirstPart(const std::string& path) 
{
	size_t pos = path.find('/');
	if (pos != std::string::npos) 
	{
		return path.substr(0, pos);

	}
	return path;
}

std::string getFileDir(const std::string& path)
{
    size_t pos = path.rfind('/');
    if (pos != std::string::npos)
    {
        return path.substr(0, pos);

    }
    return path;
}

std::vector<std::string> splitString(const std::string& s, const std::string& delimiter)
{
    size_t pos_start = 0, pos_end, delim_len = delimiter.length();
    std::string token;
    std::vector<std::string> res;

    while ((pos_end = s.find(delimiter, pos_start)) != std::string::npos) {
        token = s.substr(pos_start, pos_end - pos_start);
        pos_start = pos_end + delim_len;
        res.push_back(token);
    }

    res.push_back(s.substr(pos_start));
    return res;
}

std::string simplifyPath(const std::string& path) 
{
    if (path.empty())
        return "";

    std::vector<std::string> parts;
    std::string currentPart;
    auto pathLen = path.size();

    // 分割路径
    for (size_t i = 0; i < pathLen; ++i)
    {
        if (path[i] == '/') 
        {
            if (!currentPart.empty() && currentPart != ".")
            {
                if (currentPart == "..")
                {
                    if (parts.empty())
                        return "";
                    parts.pop_back();
                }
                else
                {
                    parts.push_back(currentPart);
                }
            }

            if (i == pathLen - 1 && !parts.empty())
            {
                parts[parts.size() - 1].append("/");
            }

            currentPart.clear();
        }
        else 
        {
            currentPart += path[i];
        }
    }
    // 最后一个部分
    if (!currentPart.empty() && currentPart != ".")
    {
        if (currentPart == "..")
        {
            if (parts.empty())
                return "";
            parts.pop_back();
        }
        else
        {
            parts.push_back(currentPart);
        }
    }

    // 构建结果路径
    std::string result = "/";
    for (size_t i = 0; i < parts.size(); ++i)
    {
        result += parts[i];
        if (i < parts.size() - 1)
        {
            result += "/";
        }
    }

    return result;
}

NS_VFS_END
