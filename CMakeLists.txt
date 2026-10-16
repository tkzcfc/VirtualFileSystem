cmake_minimum_required(VERSION 3.16)

set(lib_name vfs)
project(VirtualFileSystem VERSION 2.0.0 LANGUAGES C CXX)

# 是否作为顶层项目构建（被别的项目 add_subdirectory 时默认不构建测试和工具）
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(VFS_IS_TOP_LEVEL ON)
else()
    set(VFS_IS_TOP_LEVEL OFF)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)
include(VfsOptions)
include(VfsCompiler)

# 第三方依赖（全部 vendored 在 3rd/）
add_subdirectory(3rd)

# 运行时库
add_subdirectory(src/vfs)

# 命令行工具
if(VFS_BUILD_TOOLS)
    add_subdirectory(tools/vfstool)
endif()

# 测试
if(VFS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# 示例
if(VFS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# 性能基准
if(VFS_BUILD_BENCH)
    add_subdirectory(bench)
endif()
