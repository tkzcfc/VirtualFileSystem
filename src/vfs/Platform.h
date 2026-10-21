// This file: platform-detection macros. Library code uses VFS_PLATFORM_XXX here and does not test compiler predefined macros directly.
// Every macro is 0 or 1; test with #if VFS_PLATFORM_XXX (not #ifdef).
//
//   VFS_PLATFORM_WINDOWS  Windows (MSVC, MinGW, clang-cl)
//   VFS_PLATFORM_LINUX    desktop/server Linux (not Android)
//   VFS_PLATFORM_ANDROID  Android (NDK)
//   VFS_PLATFORM_MACOS    macOS
//   VFS_PLATFORM_IOS      iOS / iPadOS / tvOS / visionOS and their simulators
//   VFS_PLATFORM_WASM     WebAssembly (Emscripten)
//   VFS_PLATFORM_APPLE    MACOS or IOS
//   VFS_PLATFORM_POSIX    every platform except Windows (file APIs go through POSIX)
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
#elif defined(__ANDROID__)   // Android also defines __linux__, so it must be tested first
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
// Other POSIX systems (FreeBSD and so on) leave the macros above all 0, but VFS_PLATFORM_POSIX is 1, so they compile as generic POSIX.

#define VFS_PLATFORM_APPLE (VFS_PLATFORM_MACOS || VFS_PLATFORM_IOS)
#define VFS_PLATFORM_POSIX (!VFS_PLATFORM_WINDOWS)
