// This file: namespace macros, the export macro, and the common base headers for the library.
// Every vfs header should include this file first.
#pragma once

#include "Config.h"
#include "Platform.h"

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

// Export macro: vfs is built as a static library today. The macro is kept so it can later become a shared library or be folded into an engine.
#ifndef VFS_DLL
#    define VFS_DLL
#endif
