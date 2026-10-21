// doctest entry point
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"
#include "vfs/Platform.h"

#if VFS_PLATFORM_POSIX && !VFS_PLATFORM_WASM
#    include <sys/resource.h>
#endif

int main(int argc, char** argv)
{
#if VFS_PLATFORM_POSIX && !VFS_PLATFORM_WASM
	// Some platforms default to a low open-file limit (256 on the iOS simulator and in a macOS terminal),
	// while the matrix tests open 300 streams at once; raise the soft limit as far as the hard limit allows.
	struct rlimit lim;
	if (::getrlimit(RLIMIT_NOFILE, &lim) == 0 && lim.rlim_cur < 4096)
	{
		lim.rlim_cur = lim.rlim_max == RLIM_INFINITY || lim.rlim_max > 4096 ? 4096 : lim.rlim_max;
		::setrlimit(RLIMIT_NOFILE, &lim);
	}
#endif
	doctest::Context context(argc, argv);
	return context.run();
}
