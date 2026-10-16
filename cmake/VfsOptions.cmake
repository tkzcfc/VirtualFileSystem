# 构建选项。作为子项目时，工具和测试默认关闭
option(VFS_BUILD_TOOLS "构建 vfstool 命令行工具" ${VFS_IS_TOP_LEVEL})
option(VFS_BUILD_TESTS "构建 doctest 单元测试并注册到 ctest" ${VFS_IS_TOP_LEVEL})
option(VFS_BUILD_EXAMPLES "构建示例程序" OFF)
option(VFS_WITH_AXMOL "使用 axmol 的 ax::IFileStream 作为流接口（移植进 axmol 时打开）" OFF)
option(VFS_WARNINGS_AS_ERRORS "把编译警告当作错误" OFF)

# 测试依赖 vfstool_core
if(VFS_BUILD_TESTS AND NOT VFS_BUILD_TOOLS)
    message(STATUS "VFS_BUILD_TESTS 需要 vfstool_core，自动打开 VFS_BUILD_TOOLS")
    set(VFS_BUILD_TOOLS ON CACHE BOOL "构建 vfstool 命令行工具" FORCE)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
endif()
