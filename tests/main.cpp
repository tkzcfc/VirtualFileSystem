// doctest 入口
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"
#include "vfs/Platform.h"

#if VFS_PLATFORM_POSIX && !VFS_PLATFORM_WASM
#    include <sys/resource.h>
#endif

int main(int argc, char** argv)
{
#if VFS_PLATFORM_POSIX && !VFS_PLATFORM_WASM
	// 部分平台默认的打开文件数上限很低（iOS 模拟器、macOS 终端为 256），
	// 而矩阵测试会同时打开 300 个流；在硬上限允许的范围内提高软上限
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
