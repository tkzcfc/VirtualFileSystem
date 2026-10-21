# vfstool: command-line tool
# vfstool_core (static library) implements every command so vfstool and tests can call them directly

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
    # CommandLineToArgvW lives in shell32
    target_link_libraries(vfstool PRIVATE shell32)
endif()
vfs_set_warnings(vfstool)
vfs_setup_executable(vfstool)
