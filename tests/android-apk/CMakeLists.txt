# Native library of the AndroidAssetBackend test APK (see build.sh and docs/platforms.md).
cmake_minimum_required(VERSION 3.16)
project(vfsassettest LANGUAGES C CXX)

set(VFS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(VFS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. vfs)

add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(vfsassettest SHARED AssetApkTest.cpp)
target_compile_features(vfsassettest PRIVATE cxx_std_17)
target_link_libraries(vfsassettest PRIVATE vfs native_app_glue android log)
# Keep ANativeActivity_onCreate exported from the static glue library.
target_link_options(vfsassettest PRIVATE "-u" "ANativeActivity_onCreate")
