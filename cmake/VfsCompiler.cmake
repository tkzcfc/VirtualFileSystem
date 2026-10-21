# Compiler settings: C++20, warning level, and source treated as UTF-8.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add warning options to our own targets (not to third-party code).
function(vfs_set_warnings target)
    if(MSVC)
        # /utf-8: the sources contain many non-ASCII comments and must be parsed as UTF-8.
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive-)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
        if(VFS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor)
        if(VFS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

set(VFS_CMAKE_DIR ${CMAKE_CURRENT_LIST_DIR})

# Embed a longPathAware manifest in executables (MSVC only; the MSVC linker merges a .manifest source into the default manifest).
function(vfs_add_long_path_manifest target)
    if(MSVC)
        target_sources(${target} PRIVATE ${VFS_CMAKE_DIR}/longpath.manifest)
    endif()
endfunction()

# WebAssembly (Emscripten)
# - The library itself compiles without threads (std::mutex is a no-op in a threadless build), but tests, benchmarks, and the multithreaded example need pthread.
#   When VFS_WASM_THREADS=ON (the default), every target gets -pthread (targets in one program must agree, or shared memory will not link).
# - As the top-level project, executables (tests, vfstool, examples, benchmarks) link as node programs: NODERAWFS lets the native backend access
#   the host filesystem directly, and PROXY_TO_PTHREAD runs main on a worker thread (it may block waiting for other threads).
#   Browser apps should use their own link options (for example MEMFS / IDBFS / OPFS); see docs/platforms.md.
if(EMSCRIPTEN)
    option(VFS_WASM_THREADS "Build with -pthread under Emscripten" ON)
    if(VFS_WASM_THREADS)
        add_compile_options(-pthread)
        add_link_options(-pthread)
    endif()
    if(VFS_IS_TOP_LEVEL)
        # Growing memory under threads slows JS-side access (emcc warns), so pin a 3 GiB initial heap (vfs_bench write.large.seq keeps 64 MiB per thread in the memory backend, about 1 GiB at 8 threads with the layered target).
        add_link_options(-sNODERAWFS=1 -sINITIAL_MEMORY=3072MB -sSTACK_SIZE=4MB -sEXIT_RUNTIME=1)
        if(VFS_WASM_THREADS)
            add_link_options(-sPROXY_TO_PTHREAD=1 -sDEFAULT_PTHREAD_STACK_SIZE=2MB)
        endif()
    endif()
endif()

# iOS / tvOS and similar: executables are produced as .app bundles, and Info.plist needs a bundle id (CFBundleIdentifier) or the app cannot be installed on a device.
# Bundle id = VFS_APPLE_BUNDLE_ID_PREFIX + "." + target name ('_' replaced with '-'). Free developer teams require a globally unique id; change the prefix if needed.
set(VFS_APPLE_BUNDLE_ID_PREFIX "com.example.vfs" CACHE STRING "Bundle identifier prefix for executables on iOS")

# Common executable setup: Windows long-path manifest and iOS bundle id.
function(vfs_setup_executable target)
    vfs_add_long_path_manifest(${target})
    if(CMAKE_SYSTEM_NAME MATCHES "iOS|tvOS|visionOS")
        string(REPLACE "_" "-" id "${VFS_APPLE_BUNDLE_ID_PREFIX}.${target}")
        set_target_properties(${target} PROPERTIES
            MACOSX_BUNDLE ON
            MACOSX_BUNDLE_GUI_IDENTIFIER "${id}"
            MACOSX_BUNDLE_BUNDLE_NAME "${target}"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${id}")
    endif()
endfunction()
