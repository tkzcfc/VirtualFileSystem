#include "SharedMutex.h"

#if defined(_WIN32)
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>

NS_VFS_BEGIN

static_assert(sizeof(SRWLOCK) == sizeof(void*), "SRWLOCK 应当只有一个指针大小");

// m_lock 的存储直接当作 SRWLOCK 使用
#    define VFS_SRW reinterpret_cast<PSRWLOCK>(&m_lock)

void SharedMutex::lock()
{
	::AcquireSRWLockExclusive(VFS_SRW);
}

bool SharedMutex::try_lock()
{
	return ::TryAcquireSRWLockExclusive(VFS_SRW) != 0;
}

void SharedMutex::unlock()
{
	::ReleaseSRWLockExclusive(VFS_SRW);
}

void SharedMutex::lock_shared()
{
	::AcquireSRWLockShared(VFS_SRW);
}

bool SharedMutex::try_lock_shared()
{
	return ::TryAcquireSRWLockShared(VFS_SRW) != 0;
}

void SharedMutex::unlock_shared()
{
	::ReleaseSRWLockShared(VFS_SRW);
}

#    undef VFS_SRW

NS_VFS_END

#endif
