# 编译器设置：C++20、警告等级、源码按 UTF-8 处理

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 给自己的目标加警告选项（第三方代码不加）
function(vfs_set_warnings target)
    if(MSVC)
        # /utf-8：源码里有大量中文注释，必须按 UTF-8 解析
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

# 给可执行文件嵌入 longPathAware 清单（只对 MSVC；MSVC 链接器会把 .manifest 源文件合并进默认清单）
function(vfs_add_long_path_manifest target)
    if(MSVC)
        target_sources(${target} PRIVATE ${VFS_CMAKE_DIR}/longpath.manifest)
    endif()
endfunction()

# WebAssembly（Emscripten）
# - 库本身不依赖线程也能编译（std::mutex 在无线程构建中是空操作），但测试、基准和多线程示例需要 pthread；
#   VFS_WASM_THREADS=ON（默认）时所有目标统一加 -pthread（同一程序里的目标必须一致，否则无法链接共享内存）
# - 作为顶层项目时，可执行文件（测试、vfstool、示例、基准）链接成 node 程序：NODERAWFS 让本机后端直接访问
#   宿主文件系统，PROXY_TO_PTHREAD 让 main 运行在工作线程里（可以阻塞等待其他线程）。
#   浏览器中的应用请使用自己的链接选项（例如 MEMFS / IDBFS / OPFS），见 docs/platforms.md
if(EMSCRIPTEN)
    option(VFS_WASM_THREADS "Build with -pthread under Emscripten" ON)
    if(VFS_WASM_THREADS)
        add_compile_options(-pthread)
        add_link_options(-pthread)
    endif()
    if(VFS_IS_TOP_LEVEL)
        # 多线程时内存增长会让 JS 侧访问变慢（emcc 会警告），所以固定 1.5 GiB 初始内存（基准的内存后端需要约 0.5 GiB）
        add_link_options(-sNODERAWFS=1 -sINITIAL_MEMORY=1536MB -sSTACK_SIZE=4MB -sEXIT_RUNTIME=1)
        if(VFS_WASM_THREADS)
            add_link_options(-sPROXY_TO_PTHREAD=1 -sDEFAULT_PTHREAD_STACK_SIZE=2MB)
        endif()
    endif()
endif()

# iOS / tvOS 等：可执行文件生成为 .app，Info.plist 需要包标识（CFBundleIdentifier），否则无法安装到设备。
# 包标识 = VFS_APPLE_BUNDLE_ID_PREFIX + "." + 目标名（'_' 换成 '-'）。免费开发团队要求全局唯一，可以改前缀
set(VFS_APPLE_BUNDLE_ID_PREFIX "com.example.vfs" CACHE STRING "Bundle identifier prefix for executables on iOS")

# 可执行文件的通用设置：Windows 长路径清单、iOS 包标识
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
