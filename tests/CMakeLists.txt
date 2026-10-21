# Unit tests and command-line end-to-end tests
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
    archive/Lz4BlockTests.cpp
    asset/AssetTests.cpp
    crypto/CryptoTests.cpp
    tool/ToolTests.cpp
    concurrency/ConcurrencyTests.cpp
    deadlock/DeadlockTests.cpp
    matrix/Envs.h
    matrix/MatrixTests.cpp
    stress/StressTests.cpp
    model/ModelTests.cpp
    extreme/FaultBackend.h
    extreme/ExtremeTests.cpp
)
target_include_directories(vfs_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# vfs arrives through vfstool_core's PUBLIC dependency and is not linked again (otherwise Apple ld warns about duplicate libraries)
target_link_libraries(vfs_tests PRIVATE vfstool_core vfs_3rd_doctest)
vfs_set_warnings(vfs_tests)
vfs_setup_executable(vfs_tests)

find_package(Threads REQUIRED)
target_link_libraries(vfs_tests PRIVATE Threads::Threads)

# Each TEST_SUITE is registered as its own ctest case so a failure is easy to locate
foreach(suite path union native memory stream archive asset crypto tool concurrency deadlock matrix stress model extreme)
    add_test(NAME vfs.${suite} COMMAND vfs_tests --test-suite=${suite} --no-version)
endforeach()

# Command-line end to end: pack -> info -> list -> cat -> unpack -> verify, and two packs have the same hash
# When cross-compiling (node for Emscripten, wine for MinGW), run vfstool through an emulator
string(REPLACE ";" "|" VFS_E2E_EMULATOR "${CMAKE_CROSSCOMPILING_EMULATOR}")
add_test(NAME vfstool.e2e
    COMMAND ${CMAKE_COMMAND}
        -DVFSTOOL=$<TARGET_FILE:vfstool>
        "-DVFSTOOL_EMULATOR=${VFS_E2E_EMULATOR}"
        -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/data/e2e
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/e2e
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cli_e2e.cmake
)
