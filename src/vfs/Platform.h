// 本文件：平台检测宏。库内代码统一使用这里的 VFS_PLATFORM_XXX，不直接判断编译器预定义宏。
// 每个宏都定义为 0 或 1，用 #if VFS_PLATFORM_XXX 判断（不要用 #ifdef）。
//
//   VFS_PLATFORM_WINDOWS  Windows（MSVC、MinGW、clang-cl）
//   VFS_PLATFORM_LINUX    桌面/服务器 Linux（不含 Android）
//   VFS_PLATFORM_ANDROID  Android（NDK）
//   VFS_PLATFORM_MACOS    macOS
//   VFS_PLATFORM_IOS      iOS / iPadOS / tvOS / visionOS 及其模拟器
//   VFS_PLATFORM_WASM     WebAssembly（Emscripten）
//   VFS_PLATFORM_APPLE    MACOS 或 IOS
//   VFS_PLATFORM_POSIX    除 Windows 以外的所有平台（文件 API 走 POSIX）
#pragma once

#if defined(__APPLE__)
#    include <TargetConditionals.h>
#endif

#define VFS_PLATFORM_WINDOWS 0
#define VFS_PLATFORM_LINUX 0
#define VFS_PLATFORM_ANDROID 0
#define VFS_PLATFORM_MACOS 0
#define VFS_PLATFORM_IOS 0
#define VFS_PLATFORM_WASM 0

#if defined(_WIN32)
#    undef VFS_PLATFORM_WINDOWS
#    define VFS_PLATFORM_WINDOWS 1
#elif defined(__EMSCRIPTEN__)
#    undef VFS_PLATFORM_WASM
#    define VFS_PLATFORM_WASM 1
#elif defined(__ANDROID__)   // Android 同时定义了 __linux__，必须先判断
#    undef VFS_PLATFORM_ANDROID
#    define VFS_PLATFORM_ANDROID 1
#elif defined(__APPLE__)
#    if TARGET_OS_OSX
#        undef VFS_PLATFORM_MACOS
#        define VFS_PLATFORM_MACOS 1
#    else
#        undef VFS_PLATFORM_IOS
#        define VFS_PLATFORM_IOS 1
#    endif
#elif defined(__linux__)
#    undef VFS_PLATFORM_LINUX
#    define VFS_PLATFORM_LINUX 1
#endif
// 其他 POSIX 系统（FreeBSD 等）以上宏全为 0，但 VFS_PLATFORM_POSIX 为 1，按通用 POSIX 编译

#define VFS_PLATFORM_APPLE (VFS_PLATFORM_MACOS || VFS_PLATFORM_IOS)
#define VFS_PLATFORM_POSIX (!VFS_PLATFORM_WINDOWS)
