// This file: file types and metadata.
#pragma once

#include "Common.h"

NS_VFS_BEGIN

// Mount id. 0 means "not part of any mount" (for example a synthesized virtual directory).
using MountId = uint64_t;

/**
 * File type. VFS does not support symlinks, so there are only files and directories.
 */
enum class FileType : uint8_t
{
	NONE,
	FILE,
	DIR,
};

/**
 * Metadata. Permission bits and atime are not provided.
 */
struct Stat
{
	FileType type = FileType::NONE;
	int64_t size = 0;       // File size in bytes; 0 for a directory
	int64_t mtime = 0;      // Unix seconds; 0 means unknown
	MountId mount = 0;      // Mount that owns the visible entry; 0 for a synthesized virtual directory
	bool writable = false;  // Whether the layer that owns the visible entry is writable
};

NS_VFS_END
