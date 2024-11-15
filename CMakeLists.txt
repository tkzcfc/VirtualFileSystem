cmake_minimum_required(VERSION 3.1)

set(lib_name vfs)
project(${lib_name})

option(VFS_BUILD_EXAMPLES "Build the vfs example programs" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(GLOB_RECURSE VFS_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/*.h
    ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp
)

add_library(${lib_name} STATIC ${VFS_SOURCES})
target_include_directories(${lib_name} PUBLIC ${CMAKE_CURRENT_LIST_DIR}/src/)

if (VFS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()