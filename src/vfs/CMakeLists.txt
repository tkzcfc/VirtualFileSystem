# Runtime library vfs (static library)

file(GLOB_RECURSE VFS_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/*.h
    ${CMAKE_CURRENT_LIST_DIR}/*.cpp
)

add_library(${lib_name} STATIC ${VFS_SOURCES})
add_library(vfs::vfs ALIAS ${lib_name})
# Use 64-bit file offsets on 32-bit Linux as well (NativeFileStream calls lseek / ftruncate directly)
if(UNIX)
    target_compile_definitions(${lib_name} PRIVATE _FILE_OFFSET_BITS=64)
endif()

# Usage: #include "vfs/VirtualFileSystem.h"
target_include_directories(${lib_name} PUBLIC ${CMAKE_CURRENT_LIST_DIR}/..)
target_compile_features(${lib_name} PUBLIC cxx_std_20)
target_link_libraries(${lib_name} PRIVATE vfs_3rd_xxhash vfs_3rd_lz4)

if(VFS_WITH_AXMOL)
    target_compile_definitions(${lib_name} PUBLIC VFS_WITH_AXMOL=1)
endif()

# On some platforms (older libstdc++) std::filesystem must be linked separately
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
    target_link_libraries(${lib_name} PUBLIC stdc++fs)
endif()

# Android: AndroidAssetBackend uses AAssetManager (libandroid)
if(ANDROID)
    target_link_libraries(${lib_name} PUBLIC android)
endif()

find_package(Threads REQUIRED)
target_link_libraries(${lib_name} PUBLIC Threads::Threads)

vfs_set_warnings(${lib_name})
source_group(TREE ${CMAKE_CURRENT_LIST_DIR} FILES ${VFS_SOURCES})
