# 运行时库 vfs（静态库）

file(GLOB_RECURSE VFS_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/*.h
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp
)

add_library(${lib_name} STATIC ${VFS_SOURCES})
add_library(vfs::vfs ALIAS ${lib_name})
# 32 位 Linux 上也使用 64 位文件偏移（NativeFileStream 直接使用 lseek / ftruncate）
if(UNIX)
    target_compile_definitions(${lib_name} PRIVATE _FILE_OFFSET_BITS=64)
endif()

# 使用方式：#include "vfs/VirtualFileSystem.h"
target_include_directories(${lib_name} PUBLIC ${CMAKE_CURRENT_LIST_DIR}/..)
target_compile_features(${lib_name} PUBLIC cxx_std_20)
target_link_libraries(${lib_name} PRIVATE vfs_3rd_xxhash vfs_3rd_lz4)

if(VFS_WITH_AXMOL)
    target_compile_definitions(${lib_name} PUBLIC VFS_WITH_AXMOL=1)
endif()

# 部分平台（旧版 libstdc++）的 std::filesystem 需要单独链接
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
    target_link_libraries(${lib_name} PUBLIC stdc++fs)
endif()

find_package(Threads REQUIRED)
target_link_libraries(${lib_name} PUBLIC Threads::Threads)

vfs_set_warnings(${lib_name})
source_group(TREE ${CMAKE_CURRENT_LIST_DIR} FILES ${VFS_SOURCES})
