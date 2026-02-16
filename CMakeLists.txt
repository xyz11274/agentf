cmake_minimum_required(VERSION 3.10)
project(AgentFlow VERSION 0.1.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -fPIC")

# Directories
set(AGENTFLOW_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(AGENTFLOW_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(AGENTFLOW_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set(AGENTFLOW_EXAMPLE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/examples)

# Include directories
include_directories(${AGENTFLOW_INCLUDE_DIR})

# Source files
set(AGENTFLOW_FRAMEWORK_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/node.cpp
    ${AGENTFLOW_SOURCE_DIR}/edge.cpp
    ${AGENTFLOW_SOURCE_DIR}/workflow.cpp
    ${AGENTFLOW_SOURCE_DIR}/agent.cpp
)

set(AGENTFLOW_RUNTIME_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/databus.cpp
    ${AGENTFLOW_SOURCE_DIR}/agent_runtime.cpp
)

set(AGENTFLOW_BASIC_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/logger.cpp
)

set(AGENTFLOW_SOURCES
    ${AGENTFLOW_FRAMEWORK_SOURCES}
    ${AGENTFLOW_RUNTIME_SOURCES}
    ${AGENTFLOW_BASIC_SOURCES}
)

# Create main library
add_library(agentflow STATIC ${AGENTFLOW_SOURCES})
target_include_directories(agentflow PUBLIC ${AGENTFLOW_INCLUDE_DIR})

# Link with common libraries
find_package(Threads REQUIRED)
target_link_libraries(agentflow PUBLIC Threads::Threads)

# Examples
add_executable(example_basic ${AGENTFLOW_EXAMPLE_DIR}/basic_example.cpp)
target_link_libraries(example_basic PRIVATE agentflow)

# Tests
enable_testing()

# Add test executable
add_executable(test_agentflow 
    ${AGENTFLOW_TEST_DIR}/test_main.cpp
    ${AGENTFLOW_TEST_DIR}/test_agent.cpp
    ${AGENTFLOW_TEST_DIR}/test_workflow.cpp
    ${AGENTFLOW_TEST_DIR}/test_node.cpp
    ${AGENTFLOW_TEST_DIR}/test_databus.cpp
)
target_link_libraries(test_agentflow PRIVATE agentflow)
add_test(NAME AgentFlowTests COMMAND test_agentflow)

# Print configuration
message(STATUS "AgentFlow Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  CXX Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Include Dir: ${AGENTFLOW_INCLUDE_DIR}")
message(STATUS "  Source Dir: ${AGENTFLOW_SOURCE_DIR}")
