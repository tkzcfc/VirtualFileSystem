#include "Utils.h"
#include <algorithm>

NS_VFS_BEGIN

inline std::string convertPathFormatToUnixStyle(const std::string_view& path)
{
	std::string ret{ path };
	std::replace(ret.begin(), ret.end(), '\\', '/');
	return ret;
}

inline void formatDirPath(std::string& path)
{
	if (!path.empty() && path.back() != '/')
		path.push_back('/');
}

inline std::string convertDirPath(const std::string_view& path)
{
	auto outPath = convertPathFormatToUnixStyle(path);
	formatDirPath(outPath);
	return outPath;
}

inline std::string_view getFirstPart(const std::string_view& path)
{
	size_t pos = path.find('/');
	if (pos != std::string_view::npos)
	{
		return path.substr(0, pos);
	}
	return path;
}

inline std::string_view getFileDir(const std::string_view& path)
{
    size_t pos = path.rfind('/');
    if (pos != std::string::npos)
    {
        return path.substr(0, pos + 1);
    }
    return "";
}

NS_VFS_END
