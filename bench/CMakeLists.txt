# vfs_bench：性能基准（默认不构建：-DVFS_BUILD_BENCH=ON 打开）
add_executable(vfs_bench ${CMAKE_CURRENT_LIST_DIR}/vfs_bench.cpp)
target_link_libraries(vfs_bench PRIVATE vfs)
find_package(Threads REQUIRED)
target_link_libraries(vfs_bench PRIVATE Threads::Threads)
if(WIN32)
    # 读注册表获取 CPU / 系统名称
    target_link_libraries(vfs_bench PRIVATE advapi32)
endif()
if(CMAKE_SYSTEM_NAME MATCHES "^(iOS|tvOS|visionOS)$")
    # iOS 看门狗会杀掉长时间不完成启动的进程，用 UIKit 宿主入口包一层
    target_sources(vfs_bench PRIVATE ${CMAKE_CURRENT_LIST_DIR}/ios/IosMain.mm)
    target_compile_definitions(vfs_bench PRIVATE VFS_BENCH_IOS_HOST=1)
    target_link_libraries(vfs_bench PRIVATE "-framework UIKit" "-framework Foundation")
endif()
vfs_set_warnings(vfs_bench)
vfs_setup_executable(vfs_bench)
