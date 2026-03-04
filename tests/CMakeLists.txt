# AgentFlow Tests CMake Configuration

cmake_minimum_required(VERSION 3.10)
project(agentflow_tests)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Test sources
set(TEST_SOURCES
    unit/src/test_hot_reload.cpp
    unit/src/test_agent_gtest.cpp
    unit/src/test_workflow_gtest.cpp
    unit/src/test_node_gtest.cpp
    unit/src/test_databus_gtest.cpp
    unit/src/test_event_gtest.cpp
)

# Find dependencies
find_package(GTest 1.10 QUIET)
if(NOT GTest_FOUND)
    message(STATUS "GTest not found in tests/ subdir; skipping extra test targets")
    return()
endif()

# Test library (SO library for hot reload testing)
add_library(test_hot_reload_lib SHARED
    unit/data/lib/hot_reload_test.cpp
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
if(NOT TARGET test_hot_reload)
    add_executable(test_hot_reload ${TEST_SOURCES})
    target_link_libraries(test_hot_reload PRIVATE
        agentflow
        GTest::gtest
        GTest::gtest_main
        test_hot_reload_lib
    )
endif()

# Other test executables
add_executable(test_agent_gtest
    unit/src/test_agent_gtest.cpp
)
target_link_libraries(test_agent_gtest PRIVATE
    agentflow
    GTest::gtest
    GTest::gtest_main
)

add_executable(test_workflow_gtest
    unit/src/test_workflow_gtest.cpp
)
target_link_libraries(test_workflow_gtest PRIVATE
    agentflow
    GTest::gtest
    GTest::gtest_main
)

add_executable(test_node_gtest
    unit/src/test_node_gtest.cpp
)
target_link_libraries(test_node_gtest PRIVATE
    agentflow
    GTest::gtest
    GTest::gtest_main
)

add_executable(test_databus_gtest
    unit/src/test_databus_gtest.cpp
)
target_link_libraries(test_databus_gtest PRIVATE
    agentflow
    GTest::gtest
    GTest::gtest_main
)

add_executable(test_event_gtest
    unit/src/test_event_gtest.cpp
)
target_link_libraries(test_event_gtest PRIVATE
    agentflow
    GTest::gtest
    GTest::gtest_main
)

# Compiler options
if(NOT MSVC)
    if(TARGET test_hot_reload)
        target_compile_options(test_hot_reload PRIVATE -Wno-non-virtual-dtor)
    endif()
    target_compile_options(test_agent_gtest PRIVATE -Wno-non-virtual-dtor)
    target_compile_options(test_workflow_gtest PRIVATE -Wno-non-virtual-dtor)
    target_compile_options(test_node_gtest PRIVATE -Wno-non-virtual-dtor)
    target_compile_options(test_databus_gtest PRIVATE -Wno-non-virtual-dtor)
    target_compile_options(test_event_gtest PRIVATE -Wno-non-virtual-dtor)
endif()

# Add subdirectories
add_subdirectory(e2e)
# (scripts directory is currently not used)

# Testing
enable_testing()
if(TARGET test_hot_reload)
    add_test(NAME HotReloadTests COMMAND test_hot_reload)
endif()
add_test(NAME AgentTests COMMAND test_agent_gtest)
add_test(NAME WorkflowTests COMMAND test_workflow_gtest)
add_test(NAME NodeTests COMMAND test_node_gtest)
add_test(NAME DataBusTests COMMAND test_databus_gtest)
add_test(NAME EventTests COMMAND test_event_gtest)

if(AGENTFLOW_TEST_ENV)
    if(TARGET test_hot_reload)
        set_tests_properties(HotReloadTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
    endif()
    set_tests_properties(AgentTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
    set_tests_properties(WorkflowTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
    set_tests_properties(NodeTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
    set_tests_properties(DataBusTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
    set_tests_properties(EventTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
endif()

# Set RPATH for test executables so they can find shared libs at runtime
if(APPLE)
  set(_test_rpath "${CMAKE_BINARY_DIR}/lib;/usr/local/lib;/opt/homebrew/lib")
  if(DEFINED ENV{CONDA_PREFIX})
    set(_test_rpath "$ENV{CONDA_PREFIX}/lib;${_test_rpath}")
  endif()
elseif(UNIX)
  set(_test_rpath "${CMAKE_BINARY_DIR}/lib;/usr/local/lib;/usr/lib")
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} -dumpmachine
    OUTPUT_VARIABLE _gcc_multiarch
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(_gcc_multiarch AND EXISTS "/usr/lib/${_gcc_multiarch}")
    set(_test_rpath "${_test_rpath};/usr/lib/${_gcc_multiarch}")
  endif()
  if(DEFINED ENV{CONDA_PREFIX})
    set(_test_rpath "$ENV{CONDA_PREFIX}/lib;${_test_rpath}")
  endif()
endif()
if(_test_rpath)
  foreach(_tgt test_agent_gtest test_workflow_gtest test_node_gtest
               test_databus_gtest test_event_gtest)
    if(TARGET ${_tgt})
      set_target_properties(${_tgt} PROPERTIES BUILD_RPATH "${_test_rpath}")
    endif()
  endforeach()
  if(TARGET test_hot_reload)
    set_target_properties(test_hot_reload PROPERTIES BUILD_RPATH "${_test_rpath}")
  endif()
endif()

# Install targets
install(TARGETS test_hot_reload test_agent_gtest test_workflow_gtest test_node_gtest
        test_databus_gtest test_event_gtest
        RUNTIME DESTINATION bin)

# Library install
install(TARGETS test_hot_reload_lib
        LIBRARY DESTINATION lib)
