// 本文件：命名空间宏、导出宏以及全库通用的基础头文件。
// 所有 vfs 头文件都应首先包含本文件。
#pragma once

#include "Config.h"

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <functional>
#include <memory>
#include <vector>

#ifdef __cplusplus
#    define NS_VFS_BEGIN \
		namespace vfs    \
		{
#    define NS_VFS_END }
#    define USING_NS_VFS using namespace vfs
#    define NS_VFS ::vfs
#else
#    define NS_VFS_BEGIN
#    define NS_VFS_END
#    define USING_NS_VFS
#    define NS_VFS
#endif

// 导出宏：目前 vfs 只构建静态库，保留宏以便以后做成动态库或并入引擎
#ifndef VFS_DLL
#    define VFS_DLL
#endif
