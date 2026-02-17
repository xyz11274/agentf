# AgentFlow Tests CMake Configuration

cmake_minimum_required(VERSION 3.10)
project(agentflow_tests)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Test sources
set(TEST_SOURCES
    src/test_hot_reload.cpp
    src/test_agent_gtest.cpp
    src/test_workflow_gtest.cpp
    src/test_node_gtest.cpp
    src/test_databus_gtest.cpp
    src/test_event_gtest.cpp
    src/test_eventloop_gtest.cpp
)

# Find dependencies
find_package(GTest 1.10 QUIET)
if(NOT GTest_FOUND)
    message(STATUS "GTest not found in tests/ subdir; skipping extra test targets")
    return()
endif()

# Test library (SO library for hot reload testing)
add_library(test_hot_reload_lib SHARED
    lib/hot_reload_test.cpp
)

set_target_properties(test_hot_reload_lib PROPERTIES
    OUTPUT_NAME "hot_reload_test"
    PREFIX ""
    VERSION 1.0
    SOVERSION 1
)

if(APPLE)
    set_target_properties(test_hot_reload_lib PROPERTIES
        SUFFIX ".dylib"
    )
endif()

if(WIN32)
    target_compile_definitions(test_hot_reload_lib PRIVATE BUILD_DLL)
else()
    target_compile_options(test_hot_reload_lib PRIVATE -fPIC)
endif()

# Test executables
add_executable(test_hot_reload ${TEST_SOURCES})
target_link_libraries(test_hot_reload PRIVATE
    agentflow
    GTest::gtest
    GTest::gtest_main
    test_hot_reload_lib
)

# Other test executables
add_executable(test_agent_gtest
    src/test_agent_gtest.cpp
)
target_link_libraries(test_agent_gtest PRIVATE
    agentflow
    GTest::gtest
    GTest::gtest_main
)

add_executable(test_workflow_gtest
    src/test_workflow_gtest.cpp
)
target_link_libraries(test_workflow_gtest PRIVATE
    agentflow
    GTest::gtest
    GTest::gtest_main
)

add_executable(test_node_gtest
    src/test_node_gtest.cpp
)
target_link_libraries(test_node_gtest PRIVATE
    agentflow
    GTest::gtest
    GTest::gtest_main
)

add_executable(test_databus_gtest
    src/test_databus_gtest.cpp
)
target_link_libraries(test_databus_gtest PRIVATE
    agentflow
    GTest::gtest
    GTest::gtest_main
)

add_executable(test_event_gtest
    src/test_event_gtest.cpp
)
target_link_libraries(test_event_gtest PRIVATE
    agentflow
    GTest::gtest
    GTest::gtest_main
)

add_executable(test_eventloop_gtest
    src/test_eventloop_gtest.cpp
)
target_link_libraries(test_eventloop_gtest PRIVATE
    agentflow
    GTest::gtest
    GTest::gtest_main
)

# Compiler options
if(NOT MSVC)
    target_compile_options(test_hot_reload PRIVATE -Wno-non-virtual-dtor)
    target_compile_options(test_agent_gtest PRIVATE -Wno-non-virtual-dtor)
    target_compile_options(test_workflow_gtest PRIVATE -Wno-non-virtual-dtor)
    target_compile_options(test_node_gtest PRIVATE -Wno-non-virtual-dtor)
    target_compile_options(test_databus_gtest PRIVATE -Wno-non-virtual-dtor)
    target_compile_options(test_event_gtest PRIVATE -Wno-non-virtual-dtor)
    target_compile_options(test_eventloop_gtest PRIVATE -Wno-non-virtual-dtor)
endif()

# Add subdirectories
add_subdirectory(scripts)

# Testing
enable_testing()
add_test(NAME HotReloadTests COMMAND test_hot_reload)
add_test(NAME AgentTests COMMAND test_agent_gtest)
add_test(NAME WorkflowTests COMMAND test_workflow_gtest)
add_test(NAME NodeTests COMMAND test_node_gtest)
add_test(NAME DataBusTests COMMAND test_databus_gtest)
add_test(NAME EventTests COMMAND test_event_gtest)
add_test(NAME EventLoopTests COMMAND test_eventloop_gtest)

# Install targets
install(TARGETS test_hot_reload test_agent_gtest test_workflow_gtest test_node_gtest
        test_databus_gtest test_event_gtest test_eventloop_gtest
        RUNTIME DESTINATION bin)

# Library install
install(TARGETS test_hot_reload_lib
        LIBRARY DESTINATION lib)
