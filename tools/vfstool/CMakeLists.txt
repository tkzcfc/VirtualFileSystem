# vfstool：命令行工具
# vfstool_core（静态库）实现所有命令，供 vfstool 和测试直接调用

file(GLOB VFSTOOL_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_LIST_DIR}/core/*.h
    ${CMAKE_CURRENT_LIST_DIR}/core/*.cpp
)

add_library(vfstool_core STATIC ${VFSTOOL_CORE_SOURCES})
target_include_directories(vfstool_core PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(vfstool_core PUBLIC vfs)
vfs_set_warnings(vfstool_core)

add_executable(vfstool ${CMAKE_CURRENT_LIST_DIR}/main.cpp)
target_link_libraries(vfstool PRIVATE vfstool_core vfs_3rd_cli11)
if(WIN32)
    # CommandLineToArgvW 位于 shell32
    target_link_libraries(vfstool PRIVATE shell32)
endif()
vfs_set_warnings(vfstool)
