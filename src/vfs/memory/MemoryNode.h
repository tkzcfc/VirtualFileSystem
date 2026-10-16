// 本文件：内存后端的数据结构。
#pragma once

#include "../Common.h"
#include "../Stat.h"
#include <atomic>
#include <map>
#include <shared_mutex>

NS_VFS_BEGIN

/**
 * 内存文件的数据缓冲区。
 * 由文件节点和所有打开它的流共同持有（shared_ptr），所以删除文件后已打开的流仍然可用。
 * 线程安全：data 由 mutex 保护（读共享、写独占），保证内存安全；读写交错时内容由时序决定。
 */
struct MemoryBuffer
{
	mutable std::shared_mutex mutex;
	std::vector<uint8_t> data;
	std::atomic<int64_t> mtime{0};   // Unix 秒，最后一次写入的时间
};

/**
 * 内存树节点：目录（children）或文件（buffer）。
 * 树结构由 MemoryBackend 的锁保护，节点本身不加锁。
 */
struct MemoryNode
{
	FileType type = FileType::DIR;
	std::shared_ptr<MemoryBuffer> buffer;                                  // 仅文件
	std::map<std::string, std::shared_ptr<MemoryNode>, std::less<>> children; // 仅目录，按名字排序
	int64_t mtime = 0;                                                     // 仅目录
};

/**
 * 当前时间（Unix 秒）。
 */
int64_t currentUnixTime();

NS_VFS_END
