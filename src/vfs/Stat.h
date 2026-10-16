// 本文件：文件类型与元数据（决策 #27）。
#pragma once

#include "Common.h"

NS_VFS_BEGIN

// 挂载 ID。0 表示“不属于任何挂载”（例如合成的虚拟目录）
using MountId = uint64_t;

/**
 * 文件类型。VFS 不支持 symlink（决策 #20），所以只有文件和目录两种。
 */
enum class FileType : uint8_t
{
	NONE,
	FILE,
	DIR,
};

/**
 * 元数据。不提供权限位和 atime（决策 #27）。
 */
struct Stat
{
	FileType type = FileType::NONE;
	int64_t size = 0;       // 文件字节数；目录为 0
	int64_t mtime = 0;      // Unix 秒，0 表示未知
	MountId mount = 0;      // 可见项所在的挂载；合成虚拟目录为 0
	bool writable = false;  // 可见项所在的层是否可写
};

NS_VFS_END
