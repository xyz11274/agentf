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
option(USE_LIBEVENT "Enable libevent for event loop support" ON)
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
  message(STATUS "  To install: brew install openssl (macOS) or sudo apt-get install libssl-dev (Ubuntu)")
  set(USE_OPENSSL OFF)
endif()

# libwebsockets for WebSocket support
find_package(libwebsockets CONFIG QUIET
  PATHS
    "${CMAKE_INSTALL_PREFIX}"
    "${HOMEBREW_PREFIX}"
  PATH_SUFFIXES
    lib/cmake
    lib/cmake/libwebsockets
    share/cmake
)
if(libwebsockets_FOUND)
  message(STATUS "libwebsockets found - enabling WebSocket support")
  set(USE_LIBWEBSOCKETS ON)
else()
  # Try pkg-config as fallback
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(libwebsockets libwebsockets)
  endif()
  if(libwebsockets_FOUND)
    message(STATUS "libwebsockets found (via pkg-config) - enabling WebSocket support")
    set(USE_LIBWEBSOCKETS ON)
  else()
    message(STATUS "libwebsockets not found - WebSocket disabled")
    message(STATUS "  To install: brew install libwebsockets (macOS) or sudo apt-get install libwebsockets-dev (Ubuntu)")
    set(USE_LIBWEBSOCKETS OFF)
  endif()
endif()

# protobuf for serialization support
find_package(Protobuf QUIET)
if(Protobuf_FOUND)
  message(STATUS "Protobuf found - enabling serialization support")
  message(STATUS "  Protobuf Version: ${Protobuf_VERSION}")
  set(USE_PROTOBUF ON)
else()
  message(STATUS "Protobuf not found - protobuf serialization disabled")
  message(STATUS "  To install: brew install protobuf (macOS) or sudo apt-get install libprotobuf-dev protobuf-compiler (Ubuntu)")
  set(USE_PROTOBUF OFF)
endif()

# libevent for event loop support
if(USE_LIBEVENT)
  find_package(libevent CONFIG QUIET
    PATHS
      "${CMAKE_INSTALL_PREFIX}"
      "${HOMEBREW_PREFIX}"
    PATH_SUFFIXES
      lib/cmake
      lib/cmake/libevent
      share/cmake
  )
  if(libevent_FOUND)
    message(STATUS "libevent found - enabling event loop support")
  else()
    # Try pkg-config as fallback
    if(PKG_CONFIG_FOUND)
      pkg_check_modules(LIBEVENT libevent)
    endif()
    if(LIBEVENT_FOUND)
      message(STATUS "libevent found (via pkg-config) - enabling event loop support")
      # Add pkg-config variables
      if(LIBEVENT_INCLUDE_DIRS)
        include_directories(${LIBEVENT_INCLUDE_DIRS})
      endif()

      set(LIBEVENT_LIB_FIRST "")
      if(LIBEVENT_LIBRARIES)
        list(GET LIBEVENT_LIBRARIES 0 LIBEVENT_LIB_FIRST)
      endif()
      if(LIBEVENT_LIB_FIRST AND NOT IS_ABSOLUTE "${LIBEVENT_LIB_FIRST}")
        set(LIBEVENT_LIB_FROM_PKGCONFIG "")
        if(LIBEVENT_LIBRARY_DIRS)
          find_library(LIBEVENT_LIB_FROM_PKGCONFIG
            NAMES event event_core
            HINTS ${LIBEVENT_LIBRARY_DIRS}
          )
        endif()
        if(LIBEVENT_LIB_FROM_PKGCONFIG)
          set(LIBEVENT_LIBRARIES "${LIBEVENT_LIB_FROM_PKGCONFIG}")
        endif()
      endif()

      if(LIBEVENT_LIBRARIES AND NOT TARGET libevent::libevent)
        set(_libevent_pkg_path "")
        if(LIBEVENT_LIB_FIRST AND NOT IS_ABSOLUTE "${LIBEVENT_LIB_FIRST}")
          find_library(_libevent_pkg_path
            NAMES event event_core
            HINTS ${LIBEVENT_LIBRARY_DIRS}
          )
        endif()

        if(_libevent_pkg_path)
          set(LIBEVENT_LIBRARIES "${_libevent_pkg_path}")
        endif()

        if(IS_ABSOLUTE "${LIBEVENT_LIBRARIES}")
          # Create an imported target for libevent
          add_library(libevent::libevent STATIC IMPORTED)
          set_target_properties(libevent::libevent PROPERTIES
            IMPORTED_LOCATION "${LIBEVENT_LIBRARIES}"
            INTERFACE_INCLUDE_DIRECTORIES "${LIBEVENT_INCLUDE_DIRS}"
          )
        endif()
      endif()

      if(NOT TARGET libevent::libevent AND NOT (LIBEVENT_LIBRARIES AND IS_ABSOLUTE "${LIBEVENT_LIBRARIES}"))
        message(STATUS "libevent pkg-config found but no usable library; disabling event loop support")
        set(USE_LIBEVENT OFF)
      endif()
    endif()
    # Try find_library and find_path as last resort
    if(NOT TARGET libevent::libevent AND NOT LIBEVENT_LIBRARIES)
      find_library(LIBEVENT_LIB
        NAMES event event_core
        PATHS
          "${HOMEBREW_PREFIX}/lib"
          "/usr/local/lib"
          "/usr/lib"
          "/opt/homebrew/lib"
          "/opt/homebrew/lib"
          "/usr/lib/x86_64-linux-gnu"
      )
      find_path(LIBEVENT_INCLUDE_DIR
        NAMES event.h event2/event.h
        PATHS
          "${HOMEBREW_PREFIX}/include"
          "/usr/local/include"
          "/usr/include"
          "/opt/homebrew/include"
          "/usr/include/x86_64-linux-gnu"
      )
      if(LIBEVENT_LIB AND LIBEVENT_INCLUDE_DIR)
        message(STATUS "libevent found - enabling event loop support")
        message(STATUS "  libevent Library: ${LIBEVENT_LIB}")
        message(STATUS "  libevent Include: ${LIBEVENT_INCLUDE_DIR}")
        set(USE_LIBEVENT ON)
        # Create imported target
        if(NOT TARGET libevent::libevent)
          add_library(libevent::libevent STATIC IMPORTED)
          set_target_properties(libevent::libevent PROPERTIES
            IMPORTED_LOCATION "${LIBEVENT_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${LIBEVENT_INCLUDE_DIR}"
          )
        endif()
      else()
        message(STATUS "libevent not found - using stub event loop")
        message(STATUS "  To install: brew install libevent (macOS) or sudo apt-get install libevent-dev (Ubuntu)")
        set(USE_LIBEVENT OFF)
      endif()
    endif()
  endif()
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
if(RAPIDJSON_INCLUDE_DIR)
  message(STATUS "RapidJSON found - enabling JSON parsing")
  message(STATUS "  RapidJSON Include: ${RAPIDJSON_INCLUDE_DIR}")
  set(USE_RAPIDJSON ON)
else()
  message(STATUS "RapidJSON not found - using stub JSON parser")
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
    ${AGENTFLOW_SOURCE_DIR}/metrics.cpp
    ${AGENTFLOW_SOURCE_DIR}/hot_reload.cpp
    ${AGENTFLOW_SOURCE_DIR}/runtime/code_loader.cpp
    ${AGENTFLOW_SOURCE_DIR}/performance_metrics.cpp
)

set(AGENTFLOW_BASIC_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/logger.cpp
    ${AGENTFLOW_SOURCE_DIR}/thread_pool.cpp
)

set(AGENTFLOW_ACTOR_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/actor_system.cpp
    ${AGENTFLOW_SOURCE_DIR}/actor/adapter_factory.cpp
    ${AGENTFLOW_SOURCE_DIR}/actor/stub_impl.cpp
    ${AGENTFLOW_SOURCE_DIR}/node_actor.cpp
    ${AGENTFLOW_SOURCE_DIR}/workflow_actor.cpp
)

# Include Protoactor implementation only if available
if(USE_PROTOACTOR)
  list(APPEND AGENTFLOW_ACTOR_SOURCES
       ${AGENTFLOW_SOURCE_DIR}/actor/protoactor_impl.cpp
       ${AGENTFLOW_SOURCE_DIR}/actor/message_serializer.cpp
       ${AGENTFLOW_SOURCE_DIR}/actor/ask_pattern_manager.cpp
  )
endif()

# DSL/JSON sources
set(AGENTFLOW_DSL_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/dsl/json_adapter_factory.cpp
    ${AGENTFLOW_SOURCE_DIR}/dsl/json_adapter_stub.cpp
    ${AGENTFLOW_SOURCE_DIR}/dsl/workflow_parser.cpp
)

if(USE_RAPIDJSON)
  list(APPEND AGENTFLOW_DSL_SOURCES
       ${AGENTFLOW_SOURCE_DIR}/dsl/json_adapter_rapidjson.cpp
  )
endif()

# Network sources
set(AGENTFLOW_NETWORK_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/network/adapter_factory.cpp
    ${AGENTFLOW_SOURCE_DIR}/network/private_protocol.cpp
    ${AGENTFLOW_SOURCE_DIR}/network/request.cpp
    ${AGENTFLOW_SOURCE_DIR}/network/response.cpp
)

# Serialization sources
set(AGENTFLOW_SERIALIZATION_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/serialization/adapter_factory.cpp
)

# Event sources
set(AGENTFLOW_EVENT_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/event/adapter_factory.cpp
    ${AGENTFLOW_SOURCE_DIR}/event/eventloop_adapter/libevent_impl.cpp
)

# Storage sources
set(AGENTFLOW_STORAGE_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/storage/storage.cpp
    ${AGENTFLOW_SOURCE_DIR}/storage/agent_storage.cpp
    ${AGENTFLOW_SOURCE_DIR}/storage/workflow_storage.cpp
    ${AGENTFLOW_SOURCE_DIR}/storage/storage_manager.cpp
)

# Control flow sources
set(AGENTFLOW_CONTROL_FLOW_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/runtime/control_flow.cpp
)

# Zero-copy data bus sources
set(AGENTFLOW_ZERO_COPY_DATABUS_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/runtime/zero_copy_databus.cpp
    ${AGENTFLOW_SOURCE_DIR}/runtime/local_channel.cpp
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
    ${AGENTFLOW_STORAGE_SOURCES}
    ${AGENTFLOW_CONTROL_FLOW_SOURCES}
    ${AGENTFLOW_ZERO_COPY_DATABUS_SOURCES}
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
  if(TARGET websockets)
    target_link_libraries(agentflow PUBLIC websockets)
  else()
    target_link_libraries(agentflow PUBLIC ${libwebsockets_LIBRARIES})
    if(libwebsockets_INCLUDE_DIRS)
      target_include_directories(agentflow PUBLIC ${libwebsockets_INCLUDE_DIRS})
    endif()
  endif()
  target_compile_definitions(agentflow PUBLIC AGENTFLOW_WITH_LIBWEBSOCKETS=1)
endif()

# Link with Protobuf if available
if(USE_PROTOBUF)
  target_link_libraries(agentflow PUBLIC protobuf::libprotobuf)
  target_compile_definitions(agentflow PUBLIC AGENTFLOW_WITH_PROTOBUF=1)
endif()

# Link with libevent if available
if(USE_LIBEVENT)
  set(_agentflow_libevent_linked OFF)
  if(TARGET libevent::libevent)
    target_link_libraries(agentflow PUBLIC libevent::libevent)
    set(_agentflow_libevent_linked ON)
  elseif(LIBEVENT_LIBRARIES)
    set(LIBEVENT_LIB_FIRST "")
    list(GET LIBEVENT_LIBRARIES 0 LIBEVENT_LIB_FIRST)
    if(LIBEVENT_LIB_FIRST AND IS_ABSOLUTE "${LIBEVENT_LIB_FIRST}")
      target_link_libraries(agentflow PUBLIC ${LIBEVENT_LIBRARIES})
      set(_agentflow_libevent_linked ON)
    endif()
  endif()
  if(_agentflow_libevent_linked)
    target_compile_definitions(agentflow PUBLIC AGENTFLOW_WITH_LIBEVENT=1)
  else()
    message(STATUS "libevent link target not usable; disabling event loop support")
    set(USE_LIBEVENT OFF)
  endif()
endif()

# Include RapidJSON headers if available
if(USE_RAPIDJSON)
  if(RAPIDJSON_INCLUDE_DIR)
    target_include_directories(agentflow PUBLIC ${RAPIDJSON_INCLUDE_DIR})
  endif()
  target_compile_definitions(agentflow PUBLIC AGENTFLOW_WITH_RAPIDJSON=1)
endif()

# ============================================================================
# Examples
# ============================================================================
add_executable(example_basic ${AGENTFLOW_EXAMPLE_DIR}/basic_example.cpp)
target_link_libraries(example_basic PRIVATE agentflow)

# Adapter example
add_executable(example_adapter ${AGENTFLOW_EXAMPLE_DIR}/adapter_example.cpp)
target_link_libraries(example_adapter PRIVATE agentflow)

# DSL parser example
add_executable(example_dsl ${AGENTFLOW_EXAMPLE_DIR}/dsl_example.cpp)
target_link_libraries(example_dsl PRIVATE agentflow)

# Node execution example
add_executable(example_node_execution ${AGENTFLOW_EXAMPLE_DIR}/node_execution_example.cpp)
target_link_libraries(example_node_execution PRIVATE agentflow)

# Metrics collector example
add_executable(example_metrics ${AGENTFLOW_EXAMPLE_DIR}/metrics_example.cpp)
target_link_libraries(example_metrics PRIVATE agentflow)

# P1 features example
add_executable(example_p1_features ${AGENTFLOW_EXAMPLE_DIR}/p1_features_example.cpp)
target_link_libraries(example_p1_features PRIVATE agentflow)

# Storage example
add_executable(example_storage ${AGENTFLOW_EXAMPLE_DIR}/storage_example.cpp)
target_link_libraries(example_storage PRIVATE agentflow)

# Control flow example
add_executable(example_control_flow ${AGENTFLOW_EXAMPLE_DIR}/control_flow_example.cpp)
target_link_libraries(example_control_flow PRIVATE agentflow)

# ============================================================================
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
      ${AGENTFLOW_TEST_DIR}/src/test_agent_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/src/test_workflow_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/src/test_node_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/src/test_databus_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/src/test_event_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/src/test_eventloop_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/src/test_workflow_json_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/src/test_agent_json_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/src/test_local_channel_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/src/test_request_response_gtest.cpp
      ${AGENTFLOW_TEST_DIR}/src/test_hot_reload.cpp
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

  # Hot reload tests (separate executable for better organization)
    add_executable(test_hot_reload
      ${AGENTFLOW_TEST_DIR}/src/test_hot_reload.cpp
    )
  if(TARGET GTest::gtest)
    target_link_libraries(test_hot_reload PRIVATE agentflow GTest::gtest GTest::gtest_main)
  else()
    target_link_libraries(test_hot_reload PRIVATE agentflow ${GTEST_LIBRARIES})
    if(GTEST_INCLUDE_DIRS)
      target_include_directories(test_hot_reload PRIVATE ${GTEST_INCLUDE_DIRS})
    endif()
  endif()
  if(NOT MSVC)
    target_compile_options(test_hot_reload PRIVATE -Wno-non-virtual-dtor)
  endif()
  add_test(NAME HotReloadTests COMMAND test_hot_reload)
  if(AGENTFLOW_TEST_ENV)
    set_tests_properties(HotReloadTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
  endif()

  # Add tests subdirectory
  add_subdirectory(tests)

  # Adapter isolation tests with Google Test
    add_executable(test_adapter_isolation
      ${AGENTFLOW_TEST_DIR}/src/test_adapter_isolation.cpp
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
  else()
    message(STATUS "Google Test not found. Using simple test harness.")
    # Simple test harness (fallback)
    add_executable(test_agentflow_simple
      ${AGENTFLOW_TEST_DIR}/src/test_main.cpp
      ${AGENTFLOW_TEST_DIR}/src/test_agent.cpp
      ${AGENTFLOW_TEST_DIR}/src/test_workflow.cpp
      ${AGENTFLOW_TEST_DIR}/src/test_node.cpp
      ${AGENTFLOW_TEST_DIR}/src/test_databus.cpp
    )
    target_link_libraries(test_agentflow_simple PRIVATE agentflow)
    add_test(NAME AgentFlowSimpleTests COMMAND test_agentflow_simple)
    if(AGENTFLOW_TEST_ENV)
      set_tests_properties(AgentFlowSimpleTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
    endif()

    # Adapter isolation tests (standalone executable when no Google Test)
    add_executable(test_adapter_isolation
      ${AGENTFLOW_TEST_DIR}/src/test_adapter_isolation.cpp
    )
    target_link_libraries(test_adapter_isolation PRIVATE agentflow)
    add_test(NAME AdapterIsolationTests COMMAND test_adapter_isolation)
    if(AGENTFLOW_TEST_ENV)
      set_tests_properties(AdapterIsolationTests PROPERTIES ENVIRONMENT "${AGENTFLOW_TEST_ENV}")
    endif()
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
message(STATUS "  libwebsockets: ${USE_LIBWEBSOCKETS}")
message(STATUS "  Protobuf: ${USE_PROTOBUF}")
message(STATUS "  libevent: ${USE_LIBEVENT}")
message(STATUS "  RapidJSON: ${USE_RAPIDJSON}")
message(STATUS "  Google Test: ${USE_GTEST}")
message(STATUS "============================================")
message(STATUS "")
message(STATUS "To install missing dependencies:")
message(STATUS "")
message(STATUS "macOS (Homebrew):")
message(STATUS "  ./scripts/install_dependencies.sh")
message(STATUS "")
message(STATUS "Ubuntu/Debian:")
message(STATUS "  sudo apt-get install libssl-dev libwebsockets-dev libevent-dev rapidjson-dev libprotobuf-dev protobuf-compiler libgtest-dev pkg-config")
message(STATUS "")
message(STATUS "Fedora/CentOS:")
message(STATUS "  sudo dnf install openssl-devel protobuf-devel libevent-devel rapidjson-devel libwebsockets-devel gtest-devel cmake")
message(STATUS "  (or: sudo yum install openssl-devel protobuf-devel libevent-devel rapidjson-devel libwebsockets-devel gtest-devel cmake)")
message(STATUS "")
message(STATUS "Arch Linux:")
message(STATUS "  sudo pacman -S openssl protobuf libevent rapidjson libwebsockets gtest cmake")
message(STATUS "")
message(STATUS "openSUSE:")
message(STATUS "  sudo zypper install libopenssl-devel protobuf-devel libevent-devel rapidjson-devel libwebsockets-devel gtest-devel cmake")
message(STATUS "")
message(STATUS "Protoactor-cpp (all systems):")
message(STATUS "  git clone https://github.com/DanielKernel/Protoactor-cpp.git")
message(STATUS "  cd Protoactor-cpp && mkdir build && cd build && cmake .. && make && sudo make install")
message(STATUS "============================================")
