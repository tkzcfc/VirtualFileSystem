// This file: directory entry returned when listing a directory.
#pragma once

#include "Stat.h"

NS_VFS_BEGIN

/**
 * Directory entry. name is the name only (no path); when names collide, the highest-priority layer decides the type.
 */
struct DirEntry
{
	std::string name;
	FileType type = FileType::NONE;
};

NS_VFS_END
