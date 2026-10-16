// 本文件：列目录时返回的目录项。
#pragma once

#include "Stat.h"

NS_VFS_BEGIN

/**
 * 目录项。name 只是名字（不含路径），同名时由优先级最高的层决定类型。
 */
struct DirEntry
{
	std::string name;
	FileType type = FileType::NONE;
};

NS_VFS_END
