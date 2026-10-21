# Third-party dependency targets. Versions are listed in 3rd/README.md.

# xxHash: header-only (XXH_INLINE_ALL), included only from src/vfs/crypto/Hash.cpp.
add_library(vfs_3rd_xxhash INTERFACE)
target_include_directories(vfs_3rd_xxhash INTERFACE ${CMAKE_CURRENT_LIST_DIR}/xxhash)

# LZ4: built as an internal static library.
add_library(vfs_3rd_lz4 STATIC
    ${CMAKE_CURRENT_LIST_DIR}/lz4/lz4.c
    ${CMAKE_CURRENT_LIST_DIR}/lz4/lz4hc.c
)
target_include_directories(vfs_3rd_lz4 PUBLIC ${CMAKE_CURRENT_LIST_DIR}/lz4)
set_target_properties(vfs_3rd_lz4 PROPERTIES POSITION_INDEPENDENT_CODE ON FOLDER "3rd")

# doctest: tests only.
add_library(vfs_3rd_doctest INTERFACE)
target_include_directories(vfs_3rd_doctest INTERFACE ${CMAKE_CURRENT_LIST_DIR}/doctest)

# CLI11: vfstool only.
add_library(vfs_3rd_cli11 INTERFACE)
target_include_directories(vfs_3rd_cli11 SYSTEM INTERFACE ${CMAKE_CURRENT_LIST_DIR}/CLI11)
# Do not use <codecvt>: std::wstring_convert is deprecated since C++17, and libc++ warns with -Wdeprecated-declarations.
# vfstool gets UTF-8 arguments on Windows via the Win32 API itself and does not call CLI11 narrow/widen.
target_compile_definitions(vfs_3rd_cli11 INTERFACE CLI11_HAS_CODECVT=0)
