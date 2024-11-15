#pragma once

#include "Common.h"

NS_VFS_BEGIN

inline std::string convertPathFormatToUnixStyle(const std::string_view& path);

inline void formatDirPath(std::string& path);

inline std::string convertDirPath(const std::string_view& path);

inline std::string_view getFirstPart(const std::string_view& path);

inline std::string_view getFileDir(const std::string_view& path);

std::vector<std::string_view> splitString(const std::string_view& s, const std::string_view& delimiter);

std::string simplifyPath(const std::string_view& path);

NS_VFS_END

#include "Utils.inl"