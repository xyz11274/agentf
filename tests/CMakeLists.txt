# AgentFlow Tests CMake Configuration

cmake_minimum_required(VERSION 3.10)
project(agentflow_tests)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Test sources
set(TEST_SOURCES
    unit/src/test_agent_gtest.cpp
    unit/src/test_workflow_gtest.cpp
    unit/src/test_node_gtest.cpp
    unit/src/test_databus_gtest.cpp
    unit/src/test_dsl_adapter_gtest.cpp
    unit/src/test_control_message_gtest.cpp
    unit/src/test_event_handler_gtest.cpp
)

# Find dependencies
find_package(GTest 1.10 QUIET)
if(NOT GTest_FOUND)
    message(STATUS "GTest not found in tests/ subdir; skipping extra test targets")
    return()
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

add_executable(test_dsl_adapter_gtest
    unit/src/test_dsl_adapter_gtest.cpp
)
target_link_libraries(test_dsl_adapter_gtest PRIVATE
    agentflow
    GTest::gtest
    GTest::gtest_main
)

add_executable(test_control_message_gtest
    unit/src/test_control_message_gtest.cpp
)
target_link_libraries(test_control_message_gtest PRIVATE
    agentflow
    GTest::gtest
    GTest::gtest_main
)

add_executable(test_event_handler_gtest
    unit/src/test_event_handler_gtest.cpp
)
target_link_libraries(test_event_handler_gtest PRIVATE
    agentflow
    GTest::gtest
    GTest::gtest_main
)

add_executable(test_codeloader_fix_gtest
    unit/src/test_codeloader_fix_gtest.cpp
)
target_compile_definitions(test_codeloader_fix_gtest PRIVATE
    AGENTFLOW_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/include"
    AGENTFLOW_PLATFORM_INCLUDE="${CMAKE_SOURCE_DIR}/include/external"
)
target_link_libraries(test_codeloader_fix_gtest PRIVATE
    agentflow
    GTest::gtest
    GTest::gtest_main
    ${CMAKE_DL_LIBS}
)

# Compiler options
if(NOT MSVC)
    target_compile_options(test_agent_gtest PRIVATE -Wno-non-virtual-dtor)
    target_compile_options(test_workflow_gtest PRIVATE -Wno-non-virtual-dtor)
    target_compile_options(test_node_gtest PRIVATE -Wno-non-virtual-dtor)
    target_compile_options(test_databus_gtest PRIVATE -Wno-non-virtual-dtor)
    target_compile_options(test_dsl_adapter_gtest PRIVATE -Wno-non-virtual-dtor)
    target_compile_options(test_control_message_gtest PRIVATE -Wno-non-virtual-dtor)
    target_compile_options(test_event_handler_gtest PRIVATE -Wno-non-virtual-dtor)
    target_compile_options(test_codeloader_fix_gtest PRIVATE -Wno-non-virtual-dtor)
endif()

# Add subdirectories
#add_subdirectory(e2e)
# (scripts directory is currently not used)

# Testing
enable_testing()
add_test(NAME AgentTests COMMAND test_agent_gtest)
add_test(NAME WorkflowTests COMMAND test_workflow_gtest)
add_test(NAME NodeTests COMMAND test_node_gtest)
add_test(NAME DataBusTests COMMAND test_databus_gtest)
add_test(NAME DSLAdapterTests COMMAND test_dsl_adapter_gtest)
add_test(NAME ControlMessageTests COMMAND test_control_message_gtest)
add_test(NAME EventHandlerTests COMMAND test_event_handler_gtest)
add_test(NAME CodeLoaderFixTests COMMAND test_codeloader_fix_gtest)

if(AGENTFLOW_TEST_ENV)
    set_tests_properties(AgentTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
    set_tests_properties(WorkflowTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
    set_tests_properties(NodeTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
    set_tests_properties(DataBusTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
    set_tests_properties(DSLAdapterTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
    set_tests_properties(ControlMessageTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
    set_tests_properties(EventHandlerTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
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
               test_databus_gtest test_dsl_adapter_gtest test_control_message_gtest
               test_event_handler_gtest test_codeloader_fix_gtest)
    if(TARGET ${_tgt})
      set_target_properties(${_tgt} PROPERTIES BUILD_RPATH "${_test_rpath}")
    endif()
  endforeach()
endif()

# Install targets
install(TARGETS test_agent_gtest test_workflow_gtest test_node_gtest
        test_databus_gtest test_dsl_adapter_gtest test_control_message_gtest
        test_event_handler_gtest test_codeloader_fix_gtest
        RUNTIME DESTINATION bin)
