#pragma once

#include <iostream>
#include <string>
#include <functional>
#include <memory>

#ifdef __cplusplus
#    define NS_VFS_BEGIN   \
        namespace vfs \
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
