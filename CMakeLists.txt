cmake_minimum_required(VERSION 3.16)

set(lib_name vfs)
project(VirtualFileSystem VERSION 2.0.0 LANGUAGES C CXX)

# Whether this is the top-level build (when another project add_subdirectory's us, tests and tools are off by default)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(VFS_IS_TOP_LEVEL ON)
else()
    set(VFS_IS_TOP_LEVEL OFF)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
include(VfsOptions)
include(VfsCompiler)

# Third-party dependencies (all vendored under 3rd/)
add_subdirectory(3rd)

# Runtime library
add_subdirectory(src/vfs)

# Command-line tool
if(VFS_BUILD_TOOLS)
    add_subdirectory(tools/vfstool)
endif()

# Tests
if(VFS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Examples
if(VFS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Performance benchmarks
if(VFS_BUILD_BENCH)
    add_subdirectory(bench)
endif()
