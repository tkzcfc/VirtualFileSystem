# 单元测试与命令行端到端测试
add_executable(vfs_tests
    main.cpp
    TestUtils.cpp
    TestUtils.h
    data/Rfc8439Vectors.h
    path/PathTests.cpp
    union/UnionTests.cpp
    native/NativeTests.cpp
    memory/MemoryTests.cpp
    stream/StreamConformanceTests.cpp
    archive/ArchiveTests.cpp
    crypto/CryptoTests.cpp
    tool/ToolTests.cpp
    concurrency/ConcurrencyTests.cpp
    matrix/Envs.h
    matrix/MatrixTests.cpp
    stress/StressTests.cpp
    model/ModelTests.cpp
    extreme/FaultBackend.h
    extreme/ExtremeTests.cpp
)
target_include_directories(vfs_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# vfs 通过 vfstool_core 的 PUBLIC 依赖传递进来，不重复链接（否则 Apple ld 会警告 duplicate libraries）
target_link_libraries(vfs_tests PRIVATE vfstool_core vfs_3rd_doctest)
vfs_set_warnings(vfs_tests)

find_package(Threads REQUIRED)
target_link_libraries(vfs_tests PRIVATE Threads::Threads)

# 每个 TEST_SUITE 注册成一个 ctest 用例，失败时容易定位
foreach(suite path union native memory stream archive crypto tool concurrency matrix stress model extreme)
    add_test(NAME vfs.${suite} COMMAND vfs_tests --test-suite=${suite} --no-version)
endforeach()

# 命令行端到端：pack → info → list → cat → unpack → verify，以及两次打包哈希一致
add_test(NAME vfstool.e2e
    COMMAND ${CMAKE_COMMAND}
        -DVFSTOOL=$<TARGET_FILE:vfstool>
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data/e2e
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/e2e
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_e2e.cmake
)
