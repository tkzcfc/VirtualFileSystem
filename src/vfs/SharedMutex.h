// 本文件：读写锁。
// Windows 上直接用 SRWLOCK：不需要初始化和销毁，只占一个指针。MinGW 的 std::shared_mutex 基于 winpthreads 的
// pthread_rwlock，在大量节点各自持有一把锁、并发创建销毁时会崩溃（压力测试在 Wine 下复现），所以不用它。
// 其他平台就是 std::shared_mutex。两者都满足 SharedMutex 具名要求，可以配合 std::shared_lock / std::unique_lock 使用。
#pragma once

#include "Common.h"
#include <shared_mutex>

NS_VFS_BEGIN

#if defined(_WIN32)

class VFS_DLL SharedMutex
{
public:
	SharedMutex() = default;
	SharedMutex(const SharedMutex&) = delete;
	SharedMutex& operator=(const SharedMutex&) = delete;

	void lock();
	bool try_lock();
	void unlock();
	void lock_shared();
	bool try_lock_shared();
	void unlock_shared();

private:
	void* m_lock = nullptr;   // SRWLOCK（SRWLOCK_INIT 就是全 0）
};

#else

using SharedMutex = std::shared_mutex;

#endif

NS_VFS_END
