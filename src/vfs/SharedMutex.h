// This file: a reader-writer lock.
// On Windows this is SRWLOCK directly: no init or destroy, and it occupies one pointer. MinGW's std::shared_mutex is built on winpthreads'
// pthread_rwlock, which crashes when many nodes each hold a lock and are created and destroyed concurrently (reproduced by the stress test under Wine), so it is not used.
// Other platforms use std::shared_mutex. Both satisfy the SharedMutex named requirements and work with std::shared_lock / std::unique_lock.
#pragma once

#include "Common.h"
#include <shared_mutex>

NS_VFS_BEGIN

#if VFS_PLATFORM_WINDOWS

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
	void* m_lock = nullptr;   // SRWLOCK (SRWLOCK_INIT is all zeros)
};

#else

using SharedMutex = std::shared_mutex;

#endif

NS_VFS_END
