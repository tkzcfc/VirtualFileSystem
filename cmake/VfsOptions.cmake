# Build options. Tools and tests default to off when this is a subdirectory.
option(VFS_BUILD_TOOLS "Build the vfstool command-line tool" ${VFS_IS_TOP_LEVEL})
option(VFS_BUILD_TESTS "Build the doctest unit tests and register them with ctest" ${VFS_IS_TOP_LEVEL})
option(VFS_BUILD_EXAMPLES "Build the example programs" OFF)
option(VFS_BUILD_BENCH "Build the vfs_bench benchmark" OFF)
option(VFS_WITH_AXMOL "Use axmol's ax::IFileStream as the stream interface (enable when integrating with axmol)" OFF)
option(VFS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# Tests depend on vfstool_core.
if(VFS_BUILD_TESTS AND NOT VFS_BUILD_TOOLS)
    message(STATUS "VFS_BUILD_TESTS requires vfstool_core, enabling VFS_BUILD_TOOLS")
    set(VFS_BUILD_TOOLS ON CACHE BOOL "Build the vfstool command-line tool" FORCE)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
