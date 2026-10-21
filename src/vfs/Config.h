// This file: compile-time switches. Override these macros on the compiler command line or from CMake.
#pragma once

// When 1, vfs::IFileStream is axmol's ax::IFileStream (used when integrating with axmol).
#ifndef VFS_WITH_AXMOL
#    define VFS_WITH_AXMOL 0
#endif

// Maximum bytes in one read/write. The interface returns the length as int, so this must be less than INT_MAX.
#ifndef VFS_MAX_IO_CHUNK
#    define VFS_MAX_IO_CHUNK 0x7FFFF000u
#endif

// Maximum index length accepted when reading an archive index (stops a malicious file from forcing a huge allocation).
#ifndef VFS_MAX_INDEX_SIZE
#    define VFS_MAX_INDEX_SIZE (512ull * 1024ull * 1024ull)
#endif
