# vfs_bench：性能基准（默认不构建：-DVFS_BUILD_BENCH=ON 打开）
add_executable(vfs_bench ${CMAKE_CURRENT_LIST_DIR}/vfs_bench.cpp)
target_link_libraries(vfs_bench PRIVATE vfs)
find_package(Threads REQUIRED)
target_link_libraries(vfs_bench PRIVATE Threads::Threads)
vfs_set_warnings(vfs_bench)
