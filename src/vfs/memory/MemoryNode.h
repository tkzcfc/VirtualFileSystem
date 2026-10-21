// This file: data structures for the memory backend.
#pragma once

#include "../Common.h"
#include "../Stat.h"
#include <atomic>
#include <map>
#include "../SharedMutex.h"

NS_VFS_BEGIN

/**
 * Data buffer of an in-memory file.
 * Held jointly (shared_ptr) by the file node and every stream that has it open, so streams already open remain usable after the file is removed.
 * Thread safety: data is protected by mutex (shared for reads, exclusive for writes), which guarantees memory safety; interleaved reads and writes see content determined by timing.
 */
struct MemoryBuffer
{
	mutable SharedMutex mutex;
	std::vector<uint8_t> data;
	std::atomic<int64_t> mtime{0};   // Unix seconds, time of the last write
};

/**
 * In-memory tree node: a directory (children) or a file (buffer).
 * Locking: each directory node has its own reader-writer lock protecting children (mtime is atomic). type and the buffer pointer do not change after creation.
 * Lock order is always ancestors before descendants (see MemoryBackend).
 */
struct MemoryNode
{
	FileType type = FileType::DIR;
	std::shared_ptr<MemoryBuffer> buffer;                                  // files only
	std::map<std::string, std::shared_ptr<MemoryNode>, std::less<>> children; // directories only, sorted by name
	std::atomic<int64_t> mtime{0};                                         // directories only
	mutable SharedMutex mutex;                                       // directories only: protects children
};

/**
 * Current time (Unix seconds).
 */
int64_t currentUnixTime();

NS_VFS_END
