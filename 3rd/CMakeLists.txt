# 第三方依赖目标。版本见 3rd/README.md

# xxHash：header-only（XXH_INLINE_ALL），只在 src/vfs/crypto/Hash.cpp 中包含
add_library(vfs_3rd_xxhash INTERFACE)
target_include_directories(vfs_3rd_xxhash INTERFACE ${CMAKE_CURRENT_LIST_DIR}/xxhash)

# LZ4：编译成内部静态库
add_library(vfs_3rd_lz4 STATIC
    ${CMAKE_CURRENT_LIST_DIR}/lz4/lz4.c
    ${CMAKE_CURRENT_LIST_DIR}/lz4/lz4hc.c
)
target_include_directories(vfs_3rd_lz4 PUBLIC ${CMAKE_CURRENT_LIST_DIR}/lz4)
set_target_properties(vfs_3rd_lz4 PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "3rd")

# doctest：只给测试用
add_library(vfs_3rd_doctest INTERFACE)
target_include_directories(vfs_3rd_doctest INTERFACE ${CMAKE_CURRENT_LIST_DIR}/doctest)

# CLI11：只给 vfstool 用
add_library(vfs_3rd_cli11 INTERFACE)
target_include_directories(vfs_3rd_cli11 INTERFACE ${CMAKE_CURRENT_LIST_DIR}/CLI11)
