#pragma once

#include "Common.h"

NS_VFS_BEGIN

inline std::string convertPathFormatToUnixStyle(const std::string& path);

inline void formatDirPath(std::string& path);

inline std::string convertDirPath(const std::string& path);

inline std::string getFirstPart(const std::string& path);

inline std::string getFileDir(const std::string& path);

std::vector<std::string> splitString(const std::string& s, const std::string& delimiter);

std::string simplifyPath(const std::string& path);

NS_VFS_END

#include "Utils.inl"