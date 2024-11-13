#include "Utils.h"
#include <algorithm>

NS_VFS_BEGIN

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
