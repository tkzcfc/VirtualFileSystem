# Native library of the vfs_bench APK (see build.sh and docs/benchmark.md).
cmake_minimum_required(VERSION 3.16)
project(vfsbenchapk LANGUAGES C CXX)

set(VFS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(VFS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(VFS_BUILD_BENCH OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. vfs)

add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(vfsbench SHARED BenchApk.cpp ../BenchCore.cpp)
target_compile_features(vfsbench PRIVATE cxx_std_17)
target_link_libraries(vfsbench PRIVATE vfs native_app_glue android log)
# Keep ANativeActivity_onCreate exported from the static glue library.
target_link_options(vfsbench PRIVATE "-u" "ANativeActivity_onCreate")
