cmake_minimum_required(VERSION 3.10)

project(VirtualFileSystem)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


set(OutBinDir ${CMAKE_BINARY_DIR}/Bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${OutBinDir}/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${OutBinDir}/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${OutBinDir}/$<CONFIG>")
set(CMAKE_VS_DEBUGGER_WORKING_DIRECTORY "${OutBinDir}/$<CONFIG>")

# 添加glfw库
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/3rd/glfw)

# 设置编译条件
if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4 /MTd")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -std=c++11")
    if(NOT WIN32)
        set(GLAD_LIBRARIES dl)
    endif()
endif()

# 包含头文件的目录
include_directories(
    ${CMAKE_CURRENT_LIST_DIR}/3rd/glad/include/
    ${CMAKE_CURRENT_LIST_DIR}/3rd/glfw/include/
    ${CMAKE_CURRENT_LIST_DIR}/3rd/imgui/
    ${CMAKE_CURRENT_LIST_DIR}/3rd/imgui/backends/
    ${CMAKE_CURRENT_LIST_DIR}/3rd/
)

# 收集项目文件
file(
    GLOB VENDORS_SOURCES 
    ${CMAKE_CURRENT_LIST_DIR}/3rd/glad/src/glad.c
    ${CMAKE_CURRENT_LIST_DIR}/3rd/imgui/backends/imgui_impl_glfw.cpp
    ${CMAKE_CURRENT_LIST_DIR}/3rd/imgui/backends/imgui_impl_opengl3.cpp
    ${CMAKE_CURRENT_LIST_DIR}/3rd/imgui/imgui*.cpp
    ${CMAKE_CURRENT_LIST_DIR}/3rd/md5/md5.cpp
)

# 添加文件分组
source_group("vendors" FILES ${VENDORS_SOURCES})

############################################# test #############################################
set(APP_NAME test)

file(GLOB APP_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/test/*.h
    ${CMAKE_CURRENT_LIST_DIR}/test/*.cpp
    ${CMAKE_CURRENT_LIST_DIR}/3rd/md5/md5.cpp
)

# 链接可执行文件
add_executable(${APP_NAME} ${APP_SOURCES})

# 链接库
target_link_libraries(${APP_NAME} vfs)

set_target_properties(${APP_NAME} PROPERTIES
    FOLDER "apps"
)

# 在构建目标前复制资源
add_custom_command(
    TARGET ${APP_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_LIST_DIR}/test/test-data ${OutBinDir}/$<CONFIG>/test-data
    COMMENT "Copying resources to the output directory"
)

############################################# test_gui #############################################
set(APP_NAME test_gui)

file(GLOB APP_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/test_gui/*.h
    ${CMAKE_CURRENT_LIST_DIR}/test_gui/*.cpp
)

list(APPEND APP_SOURCES
    3rd/md5/md5.cpp
     )

# 链接可执行文件
add_executable(${APP_NAME} ${APP_SOURCES} ${VENDORS_SOURCES})

# 链接库
target_link_libraries(${APP_NAME} glfw vfs ${GLAD_LIBRARIES})

set_target_properties(${APP_NAME} PROPERTIES
    FOLDER "apps"
)
