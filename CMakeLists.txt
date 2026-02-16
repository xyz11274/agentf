cmake_minimum_required(VERSION 3.10)
project(AgentFlow VERSION 0.1.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -fPIC")

# Find optional dependencies (all with graceful fallback to stub implementations)

# Protoactor-cpp for Actor-based implementation
find_package(protoactor CONFIG QUIET)
if(protoactor_FOUND)
  message(STATUS "Protoactor-cpp found - enabling Actor-based implementation")
  set(USE_PROTOACTOR ON)
else()
  message(STATUS "Protoactor-cpp not found - using stub actor implementation")
  set(USE_PROTOACTOR OFF)
endif()

# OpenSSL for SSL/TLS support
find_package(OpenSSL QUIET)
if(OPENSSL_FOUND)
  message(STATUS "OpenSSL found - enabling SSL/TLS support")
  set(USE_OPENSSL ON)
else()
  message(STATUS "OpenSSL not found - SSL/TLS disabled")
  set(USE_OPENSSL OFF)
endif()

# libwebsockets for WebSocket support
find_package(libwebsockets CONFIG QUIET)
if(libwebsockets_FOUND)
  message(STATUS "libwebsockets found - enabling WebSocket support")
  set(USE_LIBWEBSOCKETS ON)
else()
  message(STATUS "libwebsockets not found - WebSocket disabled")
  set(USE_LIBWEBSOCKETS OFF)
endif()

# protobuf for serialization support
find_package(Protobuf QUIET)
if(Protobuf_FOUND)
  message(STATUS "Protobuf found - enabling serialization support")
  set(USE_PROTOBUF ON)
else()
  message(STATUS "Protobuf not found - protobuf serialization disabled")
  set(USE_PROTOBUF OFF)
endif()

# libevent for event loop support
find_package(libevent CONFIG QUIET)
if(libevent_FOUND)
  message(STATUS "libevent found - enabling event loop support")
  set(USE_LIBEVENT ON)
else()
  message(STATUS "libevent not found - using stub event loop")
  set(USE_LIBEVENT OFF)
endif()

# rapidjson for JSON parsing (header-only, commonly available)
if(NOT DEFINED RAPIDJSON_INCLUDE_DIR)
  find_path(RAPIDJSON_INCLUDE_DIR NAMES rapidjson/document.h)
endif()
if(RAPIDJSON_INCLUDE_DIR)
  message(STATUS "RapidJSON found - enabling JSON parsing")
  set(USE_RAPIDJSON ON)
else()
  message(STATUS "RapidJSON not found - using stub JSON parser")
  set(USE_RAPIDJSON OFF)
endif()

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
    ${AGENTFLOW_SOURCE_DIR}/event.cpp
    ${AGENTFLOW_SOURCE_DIR}/agent_runtime.cpp
)

set(AGENTFLOW_BASIC_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/logger.cpp
)

set(AGENTFLOW_ACTOR_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/actor/adapter_factory.cpp
    ${AGENTFLOW_SOURCE_DIR}/actor/stub_impl.cpp
)

# Include Protoactor implementation only if available
if(USE_PROTOACTOR)
  list(APPEND AGENTFLOW_ACTOR_SOURCES
       ${AGENTFLOW_SOURCE_DIR}/actor/protoactor_impl.cpp
  )
endif()

# DSL/JSON sources
set(AGENTFLOW_DSL_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/dsl/json_adapter_factory.cpp
    ${AGENTFLOW_SOURCE_DIR}/dsl/json_adapter_stub.cpp
)

if(USE_RAPIDJSON)
  list(APPEND AGENTFLOW_DSL_SOURCES
       ${AGENTFLOW_SOURCE_DIR}/dsl/json_adapter_rapidjson.cpp
  )
endif()

# Network sources
set(AGENTFLOW_NETWORK_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/network/adapter_factory.cpp
)

# Serialization sources
set(AGENTFLOW_SERIALIZATION_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/serialization/adapter_factory.cpp
)

# Event sources
set(AGENTFLOW_EVENT_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/event/adapter_factory.cpp
)

set(AGENTFLOW_SOURCES
    ${AGENTFLOW_FRAMEWORK_SOURCES}
    ${AGENTFLOW_RUNTIME_SOURCES}
    ${AGENTFLOW_BASIC_SOURCES}
    ${AGENTFLOW_ACTOR_SOURCES}
    ${AGENTFLOW_DSL_SOURCES}
    ${AGENTFLOW_NETWORK_SOURCES}
    ${AGENTFLOW_SERIALIZATION_SOURCES}
    ${AGENTFLOW_EVENT_SOURCES}
)

# Create main library
add_library(agentflow STATIC ${AGENTFLOW_SOURCES})
target_include_directories(agentflow PUBLIC ${AGENTFLOW_INCLUDE_DIR})

# Link with common libraries
find_package(Threads REQUIRED)
target_link_libraries(agentflow PUBLIC Threads::Threads)

# Link with Protoactor if available
if(USE_PROTOACTOR)
  target_link_libraries(agentflow PUBLIC protoactor::protoactor)
  target_compile_definitions(agentflow PUBLIC AGENTFLOW_WITH_PROTOACTOR=1)
endif()

# Link with OpenSSL if available
if(USE_OPENSSL)
  target_link_libraries(agentflow PUBLIC OpenSSL::SSL OpenSSL::Crypto)
  target_compile_definitions(agentflow PUBLIC AGENTFLOW_WITH_OPENSSL=1)
endif()

# Link with libwebsockets if available
if(USE_LIBWEBSOCKETS)
  target_link_libraries(agentflow PUBLIC websockets)
  target_compile_definitions(agentflow PUBLIC AGENTFLOW_WITH_LIBWEBSOCKETS=1)
endif()

# Link with Protobuf if available
if(USE_PROTOBUF)
  target_link_libraries(agentflow PUBLIC protobuf::libprotobuf)
  target_compile_definitions(agentflow PUBLIC AGENTFLOW_WITH_PROTOBUF=1)
endif()

# Link with libevent if available
if(USE_LIBEVENT)
  target_link_libraries(agentflow PUBLIC libevent::libevent)
  target_compile_definitions(agentflow PUBLIC AGENTFLOW_WITH_LIBEVENT=1)
endif()

# Include RapidJSON headers if available
if(USE_RAPIDJSON)
  if(RAPIDJSON_INCLUDE_DIR)
    target_include_directories(agentflow PUBLIC ${RAPIDJSON_INCLUDE_DIR})
  endif()
  target_compile_definitions(agentflow PUBLIC AGENTFLOW_WITH_RAPIDJSON=1)
endif()

# Examples
add_executable(example_basic ${AGENTFLOW_EXAMPLE_DIR}/basic_example.cpp)
target_link_libraries(example_basic PRIVATE agentflow)

# Adapter example
add_executable(example_adapter ${AGENTFLOW_EXAMPLE_DIR}/adapter_example.cpp)
target_link_libraries(example_adapter PRIVATE agentflow)

# Tests
enable_testing()

# Try to find Google Test, if not found, we'll use the simple test system
find_package(GTest CONFIG QUIET)

if(NOT GTest_FOUND)
  message(STATUS "Google Test not found. Install with: sudo apt-get install libgtest-dev")
  message(STATUS "Using simple test harness instead.")
  set(USE_GTEST OFF)
else()
  message(STATUS "Google Test found. Using Google Test framework.")
  set(USE_GTEST ON)
endif()

if(USE_GTEST)
  # Google Test based tests
  add_executable(test_agentflow_gtest 
      ${AGENTFLOW_TEST_DIR}/test_agent_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/test_workflow_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/test_node_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/test_databus_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/test_event_gtest.cpp
  )
  target_link_libraries(test_agentflow_gtest PRIVATE agentflow GTest::gtest GTest::gtest_main)
  if(NOT MSVC)
    target_compile_options(test_agentflow_gtest PRIVATE -Wno-non-virtual-dtor)
  endif()
  add_test(NAME AgentFlowGoogleTests COMMAND test_agentflow_gtest)
  
  # Adapter isolation tests with Google Test
  add_executable(test_adapter_isolation
      ${AGENTFLOW_TEST_DIR}/test_adapter_isolation.cpp
  )
  target_link_libraries(test_adapter_isolation PRIVATE agentflow GTest::gtest GTest::gtest_main)
  if(NOT MSVC)
    target_compile_options(test_adapter_isolation PRIVATE -Wno-non-virtual-dtor)
  endif()
  add_test(NAME AdapterIsolationTests COMMAND test_adapter_isolation)
else()
  # Simple test harness (fallback)
  add_executable(test_agentflow_simple
      ${AGENTFLOW_TEST_DIR}/test_main.cpp
      ${AGENTFLOW_TEST_DIR}/test_agent.cpp
      ${AGENTFLOW_TEST_DIR}/test_workflow.cpp
      ${AGENTFLOW_TEST_DIR}/test_node.cpp
      ${AGENTFLOW_TEST_DIR}/test_databus.cpp
  )
  target_link_libraries(test_agentflow_simple PRIVATE agentflow)
  add_test(NAME AgentFlowSimpleTests COMMAND test_agentflow_simple)
  
  # Adapter isolation tests (standalone executable when no Google Test)
  add_executable(test_adapter_isolation
      ${AGENTFLOW_TEST_DIR}/test_adapter_isolation.cpp
  )
  target_link_libraries(test_adapter_isolation PRIVATE agentflow)
  add_test(NAME AdapterIsolationTests COMMAND test_adapter_isolation)
endif()

# Print configuration
message(STATUS "AgentFlow Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  CXX Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Include Dir: ${AGENTFLOW_INCLUDE_DIR}")
message(STATUS "  Source Dir: ${AGENTFLOW_SOURCE_DIR}")
message(STATUS "Optional Dependencies:")
message(STATUS "  Protoactor-cpp: ${USE_PROTOACTOR}")
message(STATUS "  OpenSSL: ${USE_OPENSSL}")
message(STATUS "  libwebsockets: ${USE_LIBWEBSOCKETS}")
message(STATUS "  Protobuf: ${USE_PROTOBUF}")
message(STATUS "  libevent: ${USE_LIBEVENT}")
message(STATUS "  RapidJSON: ${USE_RAPIDJSON}")
message(STATUS "  Google Test: ${USE_GTEST}")
