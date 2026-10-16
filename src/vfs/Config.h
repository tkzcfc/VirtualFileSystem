// 本文件：编译期开关。可以在编译命令行或 CMake 中覆盖这些宏。
#pragma once

// 为 1 时，vfs::IFileStream 直接使用 axmol 的 ax::IFileStream（移植进 axmol 时使用）
#ifndef VFS_WITH_AXMOL
#    define VFS_WITH_AXMOL 0
#endif

// 单次 read/write 的最大字节数。接口用 int 返回读写长度，所以必须小于 INT_MAX（决策 #4）
#ifndef VFS_MAX_IO_CHUNK
#    define VFS_MAX_IO_CHUNK 0x7FFFF000u
#endif

// 读取归档索引时允许的最大索引长度（防止恶意文件导致超大内存分配）
#ifndef VFS_MAX_INDEX_SIZE
#    define VFS_MAX_INDEX_SIZE (512ull * 1024ull * 1024ull)
#endif
