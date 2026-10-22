// This file: entry point of the vfs_bench command-line tool; the benchmark itself is in BenchCore.cpp.
#include "BenchCore.h"

#ifdef VFS_BENCH_IOS_HOST
// On iOS, ios/IosMain.mm provides main and calls this after UIKit has finished launching
int vfs_bench_main(int argc, char** argv)
#else
int main(int argc, char** argv)
#endif
{
	return vfsbench::run(argc, argv);
}
