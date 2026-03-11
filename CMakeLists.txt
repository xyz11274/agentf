cmake_minimum_required(VERSION 3.10)
project(AgentFlow VERSION 0.1.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -fPIC")

# RPATH settings for proper dynamic library resolution (macOS + Linux)
set(CMAKE_MACOSX_RPATH ON)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)

# Build RPATH: ensure test/example binaries can find libs at build time
list(APPEND CMAKE_BUILD_RPATH "${CMAKE_BINARY_DIR}/lib")
if(APPLE)
  list(APPEND CMAKE_BUILD_RPATH "/usr/local/lib" "/opt/homebrew/lib")
elseif(UNIX)
  list(APPEND CMAKE_BUILD_RPATH "/usr/local/lib" "/usr/lib")
  # Add multiarch library path (e.g. /usr/lib/x86_64-linux-gnu)
  execute_process(
    COMMAND ${CMAKE_C_COMPILER} -dumpmachine
    OUTPUT_VARIABLE _gcc_multiarch
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(_gcc_multiarch AND EXISTS "/usr/lib/${_gcc_multiarch}")
    list(APPEND CMAKE_BUILD_RPATH "/usr/lib/${_gcc_multiarch}")
  endif()
endif()

# Conda environment detection (both macOS and Linux)
if(DEFINED ENV{CONDA_PREFIX})
  message(STATUS "Detected conda environment: $ENV{CONDA_PREFIX}")
  list(APPEND CMAKE_BUILD_RPATH "$ENV{CONDA_PREFIX}/lib")
  list(APPEND CMAKE_PREFIX_PATH "$ENV{CONDA_PREFIX}")
  list(APPEND CMAKE_INCLUDE_PATH "$ENV{CONDA_PREFIX}/include")
  list(APPEND CMAKE_LIBRARY_PATH "$ENV{CONDA_PREFIX}/lib")
endif()

# ============================================================================
# Homebrew support for macOS
# ============================================================================
if(APPLE)
  # Find Homebrew prefix - try common locations
  set(HOMEBREW_PREFIX "")
  if(EXISTS "/opt/homebrew")
    set(HOMEBREW_PREFIX "/opt/homebrew")
  elseif(EXISTS "/usr/local/Cellar")
    set(HOMEBREW_PREFIX "/usr/local")
  endif()

  if(HOMEBREW_PREFIX)
    message(STATUS "Found Homebrew: ${HOMEBREW_PREFIX}")
    # Add Homebrew include and lib paths
    list(APPEND CMAKE_PREFIX_PATH "${HOMEBREW_PREFIX}")
    list(APPEND CMAKE_INCLUDE_PATH "${HOMEBREW_PREFIX}/include")
    list(APPEND CMAKE_LIBRARY_PATH "${HOMEBREW_PREFIX}/lib")

    # Add OpenSSL specific paths (Homebrew keg-only)
    set(OPENSSL_ROOT_DIR "${HOMEBREW_PREFIX}/opt/openssl@3")
    if(NOT EXISTS "${OPENSSL_ROOT_DIR}")
      set(OPENSSL_ROOT_DIR "${HOMEBREW_PREFIX}/opt/openssl")
    endif()

    # Add RapidJSON path
    set(RAPIDJSON_ROOT_DIR "${HOMEBREW_PREFIX}")
  else()
    message(STATUS "Homebrew not found in standard locations")
  endif()
elseif(UNIX)
  # Linux support - add common library paths
  message(STATUS "Unix/Linux detected - adding common library paths")
  list(APPEND CMAKE_PREFIX_PATH "/usr/local")
  list(APPEND CMAKE_INCLUDE_PATH "/usr/local/include")
  list(APPEND CMAKE_LIBRARY_PATH "/usr/local/lib")
  list(APPEND CMAKE_INCLUDE_PATH "/usr/include")
  list(APPEND CMAKE_LIBRARY_PATH "/usr/lib")
endif()

# Find pkg-config for fallback package finding
find_package(PkgConfig QUIET)

# ============================================================================
# Build options
# ============================================================================
option(AGENTFLOW_BUILD_TESTS "Build unit tests" ON)

# ============================================================================
# Find optional dependencies (all with graceful fallback to stub implementations)
# ============================================================================

# Protoactor-cpp for Actor-based implementation
find_package(protoactor CONFIG QUIET
  PATHS
    "${CMAKE_INSTALL_PREFIX}"
    "${HOMEBREW_PREFIX}"
  PATH_SUFFIXES
    lib/cmake
    lib/cmake/protoactor
    share/cmake
)
if(protoactor_FOUND)
  message(STATUS "Protoactor-cpp found - enabling Actor-based implementation")
  set(USE_PROTOACTOR ON)
else()
  message(STATUS "Protoactor-cpp not found - using stub actor implementation")
  message(STATUS "  GitHub: https://github.com/DanielKernel/Protoactor-cpp.git")
  message(STATUS "  To install: git clone https://github.com/DanielKernel/Protoactor-cpp.git && cd Protoactor-cpp/build && cmake .. && make && sudo make install")
  set(USE_PROTOACTOR OFF)
endif()

# OpenSSL for SSL/TLS support
if(OPENSSL_ROOT_DIR)
  set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()
find_package(OpenSSL QUIET)
if(OPENSSL_FOUND)
  message(STATUS "OpenSSL found - enabling SSL/TLS support")
  message(STATUS "  OpenSSL Version: ${OPENSSL_VERSION}")
  set(USE_OPENSSL ON)
else()
  message(STATUS "OpenSSL not found - SSL/TLS disabled")
  message(STATUS "  GitHub: https://github.com/openssl/openssl.git")
  message(STATUS "  To install: brew install openssl (macOS) or sudo apt-get install libssl-dev (Ubuntu)")
  set(USE_OPENSSL OFF)
endif()

# rapidjson for JSON parsing (header-only, commonly available)
if(NOT DEFINED RAPIDJSON_INCLUDE_DIR)
  # RapidJSON header is at include/rapidjson/document.h
  # We need to find the parent directory of rapidjson/
  find_path(RAPIDJSON_INCLUDE_DIR
    NAMES document.h
    PATHS
      "${CMAKE_INSTALL_PREFIX}/include"
      "${HOMEBREW_PREFIX}/include"
      "/usr/local/include"
      "/usr/local/include"
      "/usr/include"
      "/opt/homebrew/include"
      "/usr/include/x86_64-linux-gnu"
    PATH_SUFFIXES rapidjson
    NO_DEFAULT_PATH
    NO_CMAKE_ENVIRONMENT_PATH
    NO_CMAKE_SYSTEM_PATH
  )
endif()

# Redis CLI (for global databus cache service integration)
find_program(REDIS_CLI_EXECUTABLE redis-cli
  PATHS
    "${HOMEBREW_PREFIX}/bin"
    "/usr/local/bin"
    "/usr/bin"
)
if(REDIS_CLI_EXECUTABLE)
  message(STATUS "Redis CLI found - enabling Redis cache adapter integration")
  set(USE_REDIS ON)
else()
  message(STATUS "Redis CLI not found - Redis cache adapter runs in local-cache mode")
  set(USE_REDIS OFF)
endif()
if(RAPIDJSON_INCLUDE_DIR)
  set(RAPIDJSON_INCLUDE_ROOT "${RAPIDJSON_INCLUDE_DIR}")
  if(EXISTS "${RAPIDJSON_INCLUDE_DIR}/document.h")
    get_filename_component(RAPIDJSON_INCLUDE_ROOT "${RAPIDJSON_INCLUDE_DIR}" DIRECTORY)
  endif()
  message(STATUS "RapidJSON found - enabling JSON parsing")
  message(STATUS "  RapidJSON Include: ${RAPIDJSON_INCLUDE_ROOT}")
  set(USE_RAPIDJSON ON)
else()
  message(STATUS "RapidJSON not found - using stub JSON parser")
  message(STATUS "  GitHub: https://github.com/Tencent/rapidjson.git")
  message(STATUS "  To install: brew install rapidjson (macOS) or sudo apt-get install rapidjson-dev (Ubuntu)")
  set(USE_RAPIDJSON OFF)
endif()

# Google Test
find_package(GTest CONFIG QUIET
  PATHS
    "${CMAKE_INSTALL_PREFIX}"
    "${HOMEBREW_PREFIX}"
  PATH_SUFFIXES
    lib/cmake
    lib/cmake/GTest
    share/cmake
)
if(NOT GTest_FOUND)
  # Try pkg-config as fallback
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(GTEST gtest)
  endif()
  if(GTEST_FOUND)
    message(STATUS "Google Test found (via pkg-config)")
    set(GTest_FOUND TRUE)
  else()
    message(STATUS "Google Test not found")
    message(STATUS "  To install: brew install googletest (macOS) or sudo apt-get install libgtest-dev cmake (Ubuntu)")
  endif()
endif()

# ============================================================================
# Directories
# ============================================================================
set(AGENTFLOW_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(AGENTFLOW_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(AGENTFLOW_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set(AGENTFLOW_EXAMPLE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/examples)
set(AGENTFLOW_VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor)

# Include directories
# Public headers (external) - for external users
include_directories(${AGENTFLOW_INCLUDE_DIR}/external)
# Internal headers - for internal implementation only
include_directories(${AGENTFLOW_INCLUDE_DIR}/internal)

# Source files - Reorganized structure

# Agent sources
set(AGENTFLOW_AGENT_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/agent/agent.cpp
    ${AGENTFLOW_SOURCE_DIR}/agent/agent_actor.cpp
    ${AGENTFLOW_SOURCE_DIR}/agent/session.cpp
    ${AGENTFLOW_SOURCE_DIR}/agent/session_router.cpp
    ${AGENTFLOW_SOURCE_DIR}/agent/task.cpp
    ${AGENTFLOW_SOURCE_DIR}/agent/msgque.cpp
    ${AGENTFLOW_SOURCE_DIR}/agent/dataref_bridge.cpp
    ${AGENTFLOW_SOURCE_DIR}/agent/coordinator.cpp
    ${AGENTFLOW_SOURCE_DIR}/agent/context.cpp
    ${AGENTFLOW_SOURCE_DIR}/agent/package.cpp
    ${AGENTFLOW_SOURCE_DIR}/agent/builtin_dispatcher.cpp
)

# Workflow sources (includes node, edge, dsl)
set(AGENTFLOW_WORKFLOW_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/workflow/workflow.cpp
    ${AGENTFLOW_SOURCE_DIR}/workflow/workflow_actor.cpp
    ${AGENTFLOW_SOURCE_DIR}/workflow/node/node.cpp
    ${AGENTFLOW_SOURCE_DIR}/workflow/node/node_actor.cpp
    ${AGENTFLOW_SOURCE_DIR}/runtime/worker/exe_loader.cpp
    ${AGENTFLOW_SOURCE_DIR}/workflow/edge/edge.cpp
    ${AGENTFLOW_SOURCE_DIR}/workflow/dsl/workflow_parser.cpp
    ${AGENTFLOW_SOURCE_DIR}/workflow/dsl/dsl_adapter.cpp
    ${AGENTFLOW_VENDOR_DIR}/adapter/json/json_adapter_factory.cpp
)

# Node adapter sources (for standalone node execution)
set(AGENTFLOW_NODE_ADAPTER_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/workflow/node/node_adapter.cpp
)

# Runtime sources
set(AGENTFLOW_RUNTIME_SOURCES
    # Actor subsystem
    ${AGENTFLOW_SOURCE_DIR}/runtime/runtime_actor.cpp
    ${AGENTFLOW_SOURCE_DIR}/runtime/worker/worker_actor.cpp
    # Scheduler
    ${AGENTFLOW_SOURCE_DIR}/runtime/runtime.cpp
    ${AGENTFLOW_SOURCE_DIR}/runtime/worker/worker.cpp
    ${AGENTFLOW_SOURCE_DIR}/runtime/master/deployer.cpp
    ${AGENTFLOW_SOURCE_DIR}/runtime/master/scheduler.cpp
    ${AGENTFLOW_TEST_DIR}/unit/data/lib/performance_metrics.cpp
    # Databus (Communication)
    ${AGENTFLOW_SOURCE_DIR}/runtime/databus/databus.cpp
    ${AGENTFLOW_SOURCE_DIR}/runtime/databus/file_databus_bridge.cpp
    ${AGENTFLOW_SOURCE_DIR}/runtime/databus/local_databus.cpp
    ${AGENTFLOW_SOURCE_DIR}/runtime/databus/remote_databus.cpp
    ${AGENTFLOW_SOURCE_DIR}/runtime/llm/streaming_client.cpp
    # Event & Trace/Metrics
    ${AGENTFLOW_SOURCE_DIR}/runtime/trace/metrics.cpp
    ${AGENTFLOW_SOURCE_DIR}/runtime/trace/trace_context.cpp
    ${AGENTFLOW_SOURCE_DIR}/runtime/trace/trace_manager.cpp
    ${AGENTFLOW_SOURCE_DIR}/runtime/trace/trace_exporter.cpp
)

# Utils sources
set(AGENTFLOW_UTILS_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/utils/logger.cpp
    ${AGENTFLOW_SOURCE_DIR}/utils/thread_pool.cpp
    ${AGENTFLOW_SOURCE_DIR}/utils/file_storage.cpp
)

# Actor adapter sources
set(AGENTFLOW_ACTOR_ADAPTER_SOURCES
    ${AGENTFLOW_VENDOR_DIR}/adapter/actor/stub_impl.cpp
    ${AGENTFLOW_VENDOR_DIR}/adapter/actor/adapter_factory.cpp
)

set(AGENTFLOW_CACHE_ADAPTER_SOURCES
    ${AGENTFLOW_VENDOR_DIR}/adapter/cache/redis_adapter.cpp
)

# Include Protoactor implementation and additional sources only if available
if(USE_PROTOACTOR)
  list(APPEND AGENTFLOW_ACTOR_ADAPTER_SOURCES
       ${AGENTFLOW_VENDOR_DIR}/adapter/actor/protoactor_impl.cpp
  )
  list(APPEND AGENTFLOW_RUNTIME_SOURCES
       ${AGENTFLOW_VENDOR_DIR}/adapter/actor/message_serializer.cpp
  )
endif()

# DSL/JSON adapter sources
set(AGENTFLOW_DSL_ADAPTER_SOURCES
    ${AGENTFLOW_VENDOR_DIR}/adapter/json/json_adapter_stub.cpp
)

if(USE_RAPIDJSON)
  list(APPEND AGENTFLOW_DSL_ADAPTER_SOURCES
       ${AGENTFLOW_VENDOR_DIR}/adapter/json/json_adapter_rapidjson.cpp
  )
endif()

if(USE_REDIS)
  target_compile_definitions(agentflow PUBLIC AGENTFLOW_WITH_REDIS=1)
endif()

set(AGENTFLOW_SOURCES
    ${AGENTFLOW_AGENT_SOURCES}
    ${AGENTFLOW_WORKFLOW_SOURCES}
    ${AGENTFLOW_RUNTIME_SOURCES}
    ${AGENTFLOW_UTILS_SOURCES}
    ${AGENTFLOW_ACTOR_ADAPTER_SOURCES}
    ${AGENTFLOW_CACHE_ADAPTER_SOURCES}
    ${AGENTFLOW_DSL_ADAPTER_SOURCES}
)

# Create main library (shared library .so)
add_library(agentflow SHARED ${AGENTFLOW_SOURCES})
target_include_directories(agentflow PUBLIC ${AGENTFLOW_INCLUDE_DIR})

# Set library output directory
set_target_properties(agentflow PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
)

# Link with common libraries
find_package(Threads REQUIRED)
target_link_libraries(agentflow PUBLIC Threads::Threads)
if(UNIX)
  target_link_libraries(agentflow PUBLIC ${CMAKE_DL_LIBS})
endif()

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

# Include RapidJSON headers if available
if(USE_RAPIDJSON)
  if(RAPIDJSON_INCLUDE_ROOT)
    target_include_directories(agentflow PUBLIC ${RAPIDJSON_INCLUDE_ROOT})
  endif()
  target_compile_definitions(agentflow PUBLIC AGENTFLOW_WITH_RAPIDJSON=1)
endif()

if(USE_REDIS)
  target_compile_definitions(node_adapter_lib PUBLIC AGENTFLOW_WITH_REDIS=1)
endif()

# ============================================================================
# Examples
# ============================================================================
# Adapter example
add_executable(example_adapter ${AGENTFLOW_EXAMPLE_DIR}/features/adapter_example.cpp)
target_link_libraries(example_adapter PRIVATE agentflow)

# Metrics collector example
add_executable(example_metrics ${AGENTFLOW_EXAMPLE_DIR}/features/metrics_example.cpp)
target_link_libraries(example_metrics PRIVATE agentflow)

# Databus example
add_executable(example_databus ${AGENTFLOW_EXAMPLE_DIR}/features/databus_example.cpp)
target_link_libraries(example_databus PRIVATE agentflow)

# Storage example
add_executable(example_storage ${AGENTFLOW_EXAMPLE_DIR}/features/storage_example.cpp)
target_link_libraries(example_storage PRIVATE agentflow)

# DSL Adapter example
add_executable(example_dsl_adapter ${AGENTFLOW_EXAMPLE_DIR}/features/dsl_adapter_example.cpp)
target_link_libraries(example_dsl_adapter PRIVATE agentflow)

# Agent Runner - Standalone executable for loading and executing Agent packages
add_executable(agent_runner
    ${AGENTFLOW_EXAMPLE_DIR}/features/agent_runner.cpp
    ${AGENTFLOW_EXAMPLE_DIR}/features/interactive_runner.cpp
)
target_link_libraries(agent_runner PRIVATE agentflow)

add_executable(example_claw_agent_tui
    ${AGENTFLOW_EXAMPLE_DIR}/agent/claw_agent/claw_agent_tui.cpp
    ${AGENTFLOW_EXAMPLE_DIR}/features/interactive_runner.cpp
)
target_link_libraries(example_claw_agent_tui PRIVATE agentflow)

add_executable(example_multi_session_chat_tui
    ${AGENTFLOW_EXAMPLE_DIR}/agent/multi_session_chat/multi_session_chat_tui.cpp
    ${AGENTFLOW_EXAMPLE_DIR}/features/interactive_runner.cpp
)
target_link_libraries(example_multi_session_chat_tui PRIVATE agentflow)

add_executable(example_openjiuwen_multi_session_chat_tui
    ${AGENTFLOW_EXAMPLE_DIR}/agent/openjiuwen_multi_session_chat/openjiuwen_multi_session_chat_tui.cpp
    ${AGENTFLOW_EXAMPLE_DIR}/features/interactive_runner.cpp
)
target_link_libraries(example_openjiuwen_multi_session_chat_tui PRIVATE agentflow)

add_executable(example_openjiuwen_studio_agent_tui
    ${AGENTFLOW_EXAMPLE_DIR}/agent/openjiuwen_studio_agent/openjiuwen_studio_agent_tui.cpp
    ${AGENTFLOW_EXAMPLE_DIR}/features/interactive_runner.cpp
)
target_link_libraries(example_openjiuwen_studio_agent_tui PRIVATE agentflow)

add_executable(example_llm_query_agent_tui
    ${AGENTFLOW_EXAMPLE_DIR}/agent/llm_query_agent/llm_query_agent_tui.cpp
    ${AGENTFLOW_EXAMPLE_DIR}/features/interactive_runner.cpp
)
target_link_libraries(example_llm_query_agent_tui PRIVATE agentflow)

# multi_session_chat polyglot SO library — C++ nodes for dispatcher/coordinator/session-dispatcher workflows
add_library(multi_session_nodes SHARED
    ${AGENTFLOW_EXAMPLE_DIR}/agent/multi_session_chat/nodes/cpp/multi_session_nodes.cpp)
target_include_directories(multi_session_nodes PRIVATE ${AGENTFLOW_INCLUDE_DIR})
target_link_libraries(multi_session_nodes PRIVATE agentflow)
set_target_properties(multi_session_nodes PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ============================================================================
# LLM Query Agent - C++ SO Library node for LLM queries
# ============================================================================
set(LLM_QUERY_DIR "/root/projects/llm-query")
set(LLM_QUERY_INCLUDE_DIR "${LLM_QUERY_DIR}/include")
set(LLM_QUERY_LIB_DIR "${LLM_QUERY_DIR}/build/src/lib")
set(LLM_QUERY_JSON_INCLUDE_DIR "${LLM_QUERY_DIR}/build/_deps/json-src/include")

# Check if llm-query library exists
if(EXISTS "${LLM_QUERY_LIB_DIR}/libllm-query.so")
  message(STATUS "llm-query library found - enabling LLM Query Agent node")

  add_library(llm_query_node SHARED
      ${AGENTFLOW_EXAMPLE_DIR}/agent/llm_query_agent/nodes/llm_node.cpp)

  target_include_directories(llm_query_node PRIVATE
      ${AGENTFLOW_INCLUDE_DIR}
      ${LLM_QUERY_INCLUDE_DIR}
      ${LLM_QUERY_JSON_INCLUDE_DIR})

  # Link with agentflow (for DataMap types) and llm-query
  target_link_libraries(llm_query_node PRIVATE
      agentflow
      ${LLM_QUERY_LIB_DIR}/libllm-query.so)

  set_target_properties(llm_query_node PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

  # Add RPATH for llm-query library
  set_target_properties(llm_query_node PROPERTIES
      BUILD_RPATH "${LLM_QUERY_LIB_DIR};${CMAKE_BINARY_DIR}/lib"
      INSTALL_RPATH "${LLM_QUERY_LIB_DIR}")
else()
  message(STATUS "llm-query library not found at ${LLM_QUERY_LIB_DIR} - skipping LLM Query Agent node")
  message(STATUS "  To enable: build llm-query library from https://github.com/your-repo/llm-query")
endif()

# ============================================================================
# Node Adapter Library and Executable
# ============================================================================

# Node adapter shared library - needs all AgentFlow sources including agent
add_library(node_adapter_lib SHARED
    ${AGENTFLOW_AGENT_SOURCES}
    ${AGENTFLOW_WORKFLOW_SOURCES}
    ${AGENTFLOW_RUNTIME_SOURCES}
    ${AGENTFLOW_UTILS_SOURCES}
    ${AGENTFLOW_ACTOR_ADAPTER_SOURCES}
    ${AGENTFLOW_CACHE_ADAPTER_SOURCES}
    ${AGENTFLOW_DSL_ADAPTER_SOURCES})
target_include_directories(node_adapter_lib PUBLIC ${AGENTFLOW_INCLUDE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(node_adapter_lib PUBLIC Threads::Threads)
if(UNIX)
  target_link_libraries(node_adapter_lib PUBLIC ${CMAKE_DL_LIBS})
endif()

# Link with Protoactor if available
if(USE_PROTOACTOR)
  target_link_libraries(node_adapter_lib PUBLIC protoactor::protoactor)
  target_compile_definitions(node_adapter_lib PUBLIC AGENTFLOW_WITH_PROTOACTOR=1)
endif()

# Link with OpenSSL if available
if(USE_OPENSSL)
  target_link_libraries(node_adapter_lib PUBLIC OpenSSL::SSL OpenSSL::Crypto)
  target_compile_definitions(node_adapter_lib PUBLIC AGENTFLOW_WITH_OPENSSL=1)
endif()

# Include RapidJSON headers if available
if(USE_RAPIDJSON)
  if(RAPIDJSON_INCLUDE_ROOT)
    target_include_directories(node_adapter_lib PUBLIC ${RAPIDJSON_INCLUDE_ROOT})
  endif()
  target_compile_definitions(node_adapter_lib PUBLIC AGENTFLOW_WITH_RAPIDJSON=1)
endif()

# Node adapter executable
add_executable(node_adapter_bin ${AGENTFLOW_NODE_ADAPTER_SOURCES})
target_link_libraries(node_adapter_bin PRIVATE node_adapter_lib)
target_include_directories(node_adapter_bin PRIVATE ${AGENTFLOW_INCLUDE_DIR})



# Tests
# ============================================================================
if(AGENTFLOW_BUILD_TESTS)
  enable_testing()

  # Set runtime library search path for test binaries
  if(UNIX AND NOT APPLE)
    set(AGENTFLOW_TEST_LD_LIBRARY_PATH "${CMAKE_BINARY_DIR}/lib:/usr/lib/x86_64-linux-gnu:/usr/lib:/lib")
    if(DEFINED ENV{CONDA_PREFIX})
      set(AGENTFLOW_TEST_LD_LIBRARY_PATH "$ENV{CONDA_PREFIX}/lib:${AGENTFLOW_TEST_LD_LIBRARY_PATH}")
    endif()
    set(AGENTFLOW_TEST_ENV "LD_LIBRARY_PATH=${AGENTFLOW_TEST_LD_LIBRARY_PATH}")
  elseif(APPLE)
    set(AGENTFLOW_TEST_DYLD_PATH "${CMAKE_BINARY_DIR}/lib:/usr/local/lib:/opt/homebrew/lib")
    if(DEFINED ENV{CONDA_PREFIX})
      set(AGENTFLOW_TEST_DYLD_PATH "$ENV{CONDA_PREFIX}/lib:${AGENTFLOW_TEST_DYLD_PATH}")
    endif()
    set(AGENTFLOW_TEST_ENV "DYLD_LIBRARY_PATH=${AGENTFLOW_TEST_DYLD_PATH}")
  endif()

  set(USE_GTEST ${GTest_FOUND})

  if(USE_GTEST)
  message(STATUS "Google Test found. Using Google Test framework.")
  # Google Test based tests
  add_executable(test_agentflow_gtest
      ${AGENTFLOW_TEST_DIR}/unit/src/test_agent_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/unit/src/test_workflow_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/unit/src/test_node_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/unit/src/test_databus_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/unit/src/test_workflow_json_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/unit/src/test_agent_json_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/unit/src/test_local_channel_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/unit/src/test_runtime_master_worker_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/unit/src/test_builtin_workflow_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/unit/src/test_dsl_adapter_gtest.cpp
  )
  if(TARGET GTest::gtest)
    target_link_libraries(test_agentflow_gtest PRIVATE agentflow GTest::gtest GTest::gtest_main)
  else()
    target_link_libraries(test_agentflow_gtest PRIVATE agentflow ${GTEST_LIBRARIES})
    if(GTEST_INCLUDE_DIRS)
      target_include_directories(test_agentflow_gtest PRIVATE ${GTEST_INCLUDE_DIRS})
    endif()
  endif()
  if(NOT MSVC)
    target_compile_options(test_agentflow_gtest PRIVATE -Wno-non-virtual-dtor)
  endif()
  add_test(NAME AgentFlowGoogleTests COMMAND test_agentflow_gtest)
  if(AGENTFLOW_TEST_ENV)
    set_tests_properties(AgentFlowGoogleTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
  endif()

  # Add tests subdirectory
  add_subdirectory(tests)

  # Adapter isolation tests with Google Test
    add_executable(test_adapter_isolation
      ${AGENTFLOW_TEST_DIR}/unit/src/test_adapter_isolation.cpp
    )
  if(TARGET GTest::gtest)
    target_link_libraries(test_adapter_isolation PRIVATE agentflow GTest::gtest GTest::gtest_main)
  else()
    target_link_libraries(test_adapter_isolation PRIVATE agentflow ${GTEST_LIBRARIES})
    if(GTEST_INCLUDE_DIRS)
      target_include_directories(test_adapter_isolation PRIVATE ${GTEST_INCLUDE_DIRS})
    endif()
  endif()
  if(NOT MSVC)
    target_compile_options(test_adapter_isolation PRIVATE -Wno-non-virtual-dtor)
  endif()
  add_test(NAME AdapterIsolationTests COMMAND test_adapter_isolation)
  if(AGENTFLOW_TEST_ENV)
    set_tests_properties(AdapterIsolationTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
  endif()

  # Agent loading tests
  add_executable(test_agent_loading
      tests/functional/src/test_agent_loading.cpp
  )
  if(TARGET GTest::gtest)
    target_link_libraries(test_agent_loading PRIVATE agentflow GTest::gtest GTest::gtest_main)
  else()
    target_link_libraries(test_agent_loading PRIVATE agentflow ${GTEST_LIBRARIES})
    if(GTEST_INCLUDE_DIRS)
      target_include_directories(test_agent_loading PRIVATE ${GTEST_INCLUDE_DIRS})
    endif()
  endif()
  if(NOT MSVC)
    target_compile_options(test_agent_loading PRIVATE -Wno-non-virtual-dtor)
  endif()
  add_test(NAME AgentLoadingTests COMMAND test_agent_loading)
  if(AGENTFLOW_TEST_ENV)
    set_tests_properties(AgentLoadingTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
  endif()
  else()
    message(STATUS "Google Test not found. Using simple test harness.")
    # Simple test harness (fallback)
    add_executable(test_agentflow_simple
      ${AGENTFLOW_TEST_DIR}/unit/src/test_main.cpp
      ${AGENTFLOW_TEST_DIR}/unit/src/test_agent.cpp
      ${AGENTFLOW_TEST_DIR}/unit/src/test_workflow.cpp
      ${AGENTFLOW_TEST_DIR}/unit/src/test_node.cpp
      ${AGENTFLOW_TEST_DIR}/unit/src/test_databus.cpp
    )
    target_link_libraries(test_agentflow_simple PRIVATE agentflow)
    add_test(NAME AgentFlowSimpleTests COMMAND test_agentflow_simple)
    if(AGENTFLOW_TEST_ENV)
      set_tests_properties(AgentFlowSimpleTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
    endif()
    # Note: test_adapter_isolation requires Google Test and is skipped when GTest is not available
  endif()
else()
  message(STATUS "Tests disabled (AGENTFLOW_BUILD_TESTS=OFF)")
endif()

# ============================================================================
# Print configuration summary
# ============================================================================
message(STATUS "")
message(STATUS "============================================")
message(STATUS "AgentFlow Configuration Summary")
message(STATUS "============================================")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  CXX Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Include Dir: ${AGENTFLOW_INCLUDE_DIR}")
message(STATUS "  Source Dir: ${AGENTFLOW_SOURCE_DIR}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "")
message(STATUS "Optional Dependencies:")
message(STATUS "  Protoactor-cpp: ${USE_PROTOACTOR}")
message(STATUS "  OpenSSL: ${USE_OPENSSL}")
message(STATUS "  RapidJSON: ${USE_RAPIDJSON}")
message(STATUS "  Google Test: ${USE_GTEST}")
message(STATUS "============================================")
message(STATUS "")
message(STATUS "To install missing dependencies:")
message(STATUS "")
message(STATUS "macOS (Homebrew):")
message(STATUS "  ./build/install_dependencies.sh")
message(STATUS "")
message(STATUS "Ubuntu/Debian:")
message(STATUS "  sudo apt-get install libssl-dev rapidjson-dev libgtest-dev pkg-config")
message(STATUS "")
message(STATUS "Fedora/CentOS:")
message(STATUS "  sudo dnf install openssl-devel rapidjson-devel gtest-devel cmake")
message(STATUS "  (or: sudo yum install openssl-devel rapidjson-devel gtest-devel cmake)")
message(STATUS "")
message(STATUS "Arch Linux:")
message(STATUS "  sudo pacman -S openssl rapidjson gtest cmake")
message(STATUS "")
message(STATUS "openSUSE:")
message(STATUS "  sudo zypper install libopenssl-devel rapidjson-devel gtest-devel cmake")
message(STATUS "")
message(STATUS "Protoactor-cpp (all systems):")
message(STATUS "  GitHub: https://github.com/DanielKernel/Protoactor-cpp.git")
message(STATUS "  git clone https://github.com/DanielKernel/Protoactor-cpp.git")
message(STATUS "  cd Protoactor-cpp && mkdir build && cd build && cmake .. && make && sudo make install")
message(STATUS "")
message(STATUS "Third-party Library GitHub Repositories:")
message(STATUS "  Protoactor-cpp: https://github.com/DanielKernel/Protoactor-cpp.git")
message(STATUS "  OpenSSL: https://github.com/openssl/openssl.git")
message(STATUS "  RapidJSON: https://github.com/Tencent/rapidjson.git")
message(STATUS "============================================")
