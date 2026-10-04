# White-box tests for the MCP bridge's path and name handling. Following
# test_filmicrgb.c, dt_bridge.c is #included directly for access to its static
# helpers, so the test needs the same json-glib flags the mcp target uses.
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSON_GLIB REQUIRED json-glib-1.0)

add_cmocka_test(test_mcp_paths
                SOURCES test_mcp_paths.c ${CMAKE_SOURCE_DIR}/src/mcp/mcp_params.c
                LINK_LIBRARIES lib_darktable whereami cmocka ${JSON_GLIB_LIBRARIES})

add_cmocka_test(test_mcp_params
                SOURCES test_mcp_params.c ${CMAKE_SOURCE_DIR}/src/mcp/mcp_params.c
                LINK_LIBRARIES lib_darktable cmocka ${JSON_GLIB_LIBRARIES})

target_include_directories(test_mcp_paths PRIVATE ${JSON_GLIB_INCLUDE_DIRS})
target_link_directories(test_mcp_paths PRIVATE ${JSON_GLIB_LIBRARY_DIRS})

target_include_directories(test_mcp_params PRIVATE ${JSON_GLIB_INCLUDE_DIRS})
target_link_directories(test_mcp_params PRIVATE ${JSON_GLIB_LIBRARY_DIRS})

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME test_mcp_engine_contract
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_mcp_engine_contract.py
                 $<TARGET_FILE:darktable-mcp>)
set_tests_properties(test_mcp_engine_contract PROPERTIES TIMEOUT 180)


if(WIN32)
    _copy_required_library(test_mcp_paths lib_darktable)
    _copy_required_library(test_mcp_params lib_darktable)
endif(WIN32)
