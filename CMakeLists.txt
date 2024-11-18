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

find_package(ZLIB QUIET)
if(ZLIB_FOUND)
    message(STATUS "Found ZLIB: ${ZLIB_INCLUDE_DIRS}")
    message(STATUS "ZLIB libraries: ${ZLIB_LIBRARIES}")
    add_definitions(-DVFS_HAS_ZLIB)
    target_include_directories(${lib_name} PRIVATE ${ZLIB_INCLUDE_DIRS})
    target_link_libraries(${lib_name} PRIVATE ZLIB::ZLIB)
else()
    message(STATUS "ZLIB not found!")
endif()

if (VFS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()