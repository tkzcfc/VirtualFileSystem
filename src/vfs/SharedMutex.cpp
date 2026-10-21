#include "SharedMutex.h"

#if VFS_PLATFORM_WINDOWS
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>

NS_VFS_BEGIN

static_assert(sizeof(SRWLOCK) == sizeof(void*), "SRWLOCK is expected to be pointer-sized");

// m_lock's storage is used directly as an SRWLOCK
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
