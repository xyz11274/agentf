# AgentFlow Dependencies Management
#
# This module manages all external dependencies for the AgentFlow project.
# Dependencies are organized into required and optional categories.

include(FindPkgConfig)

# ==============================================================================
# Required Dependencies
# ==============================================================================

# C++17 Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Threading
find_package(Threads REQUIRED)

# ==============================================================================
# Optional Dependencies - Core
# ==============================================================================

# Protoactor-cpp for Actor-based implementation
option(AGENTFLOW_WITH_PROTOACTOR "Enable Protoactor-cpp for Actor system" ON)
if(AGENTFLOW_WITH_PROTOACTOR)
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
        set(AGENTFLOW_HAS_PROTOACTOR ON)
    else()
        message(STATUS "Protoactor-cpp not found - using stub implementation")
        set(AGENTFLOW_HAS_PROTOACTOR OFF)
    endif()
endif()

# ==============================================================================
# Optional Dependencies - Network
# ==============================================================================

# OpenSSL for SSL/TLS support
option(AGENTFLOW_WITH_OPENSSL "Enable OpenSSL for SSL/TLS support" ON)
if(AGENTFLOW_WITH_OPENSSL)
    find_package(OpenSSL QUIET)
    if(OpenSSL_FOUND)
        message(STATUS "OpenSSL found - enabling SSL/TLS support")
        set(AGENTFLOW_HAS_OPENSSL ON)
    else()
        message(STATUS "OpenSSL not found - SSL/TLS disabled")
        set(AGENTFLOW_HAS_OPENSSL OFF)
    endif()
endif()

# libwebsockets for WebSocket support
option(AGENTFLOW_WITH_LIBWEBSOCKETS "Enable libwebsockets for WebSocket support" ON)
if(AGENTFLOW_WITH_LIBWEBSOCKETS)
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
        set(AGENTFLOW_HAS_LIBWEBSOCKETS ON)
    else()
        message(STATUS "libwebsockets not found - WebSocket disabled")
        set(AGENTFLOW_HAS_LIBWEBSOCKETS OFF)
    endif()
endif()

# ==============================================================================
# Optional Dependencies - Serialization
# ==============================================================================

# Protobuf for serialization
option(AGENTFLOW_WITH_PROTOBUF "Enable Protobuf for serialization" ON)
if(AGENTFLOW_WITH_PROTOBUF)
    find_package(Protobuf QUIET)
    if(Protobuf_FOUND)
        message(STATUS "Protobuf found - enabling serialization support")
        set(AGENTFLOW_HAS_PROTOBUF ON)
    else()
        message(STATUS "Protobuf not found - serialization disabled")
        set(AGENTFLOW_HAS_PROTOBUF OFF)
    endif()
endif()

# RapidJSON for JSON parsing
option(AGENTFLOW_WITH_RAPIDJSON "Enable RapidJSON for JSON parsing" ON)
if(AGENTFLOW_WITH_RAPIDJSON)
    find_path(RAPIDJSON_INCLUDE_DIR
        NAMES document.h
        PATHS
            "${CMAKE_INSTALL_PREFIX}/include"
            "${HOMEBREW_PREFIX}/include"
            "/usr/local/include"
            "/usr/include"
            "/opt/homebrew/include"
        PATH_SUFFIXES rapidjson
        NO_DEFAULT_PATH
    )
    if(RAPIDJSON_INCLUDE_DIR)
        message(STATUS "RapidJSON found - enabling JSON parsing")
        set(AGENTFLOW_HAS_RAPIDJSON ON)
    else()
        message(STATUS "RapidJSON not found - using stub JSON parser")
        set(AGENTFLOW_HAS_RAPIDJSON OFF)
    endif()
endif()

# ==============================================================================
# Optional Dependencies - Event Loop
# ==============================================================================

# libevent for event loop
option(AGENTFLOW_WITH_LIBEVENT "Enable libevent for event loop" ON)
if(AGENTFLOW_WITH_LIBEVENT)
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
        set(AGENTFLOW_HAS_LIBEVENT ON)
    else()
        # Try pkg-config as fallback
        pkg_check_modules(LIBEVENT libevent)
        if(LIBEVENT_FOUND)
            message(STATUS "libevent found (via pkg-config) - enabling event loop support")
            set(AGENTFLOW_HAS_LIBEVENT ON)
        else()
            # Try find_library as last resort
            find_library(LIBEVENT_LIB
                NAMES event event_core
                PATHS
                    "${HOMEBREW_PREFIX}/lib"
                    "/usr/local/lib"
                    "/usr/lib"
                    "/opt/homebrew/lib"
            )
            find_path(LIBEVENT_INCLUDE_DIR
                NAMES event.h event2/event.h
                PATHS
                    "${HOMEBREW_PREFIX}/include"
                    "/usr/local/include"
                    "/usr/include"
                    "/opt/homebrew/include"
            )
            if(LIBEVENT_LIB AND LIBEVENT_INCLUDE_DIR)
                message(STATUS "libevent found - enabling event loop support")
                set(AGENTFLOW_HAS_LIBEVENT ON)
            else()
                message(STATUS "libevent not found - using stub event loop")
                set(AGENTFLOW_HAS_LIBEVENT OFF)
            endif()
        endif()
    endif()
endif()

# ==============================================================================
# Test Dependencies
# ==============================================================================

option(AGENTFLOW_BUILD_TESTS "Build unit tests" ON)
if(AGENTFLOW_BUILD_TESTS)
    find_package(GTest CONFIG QUIET
        PATHS
            "${CMAKE_INSTALL_PREFIX}"
            "${HOMEBREW_PREFIX}"
        PATH_SUFFIXES
            lib/cmake
            lib/cmake/GTest
            share/cmake
    )
    if(GTest_FOUND)
        message(STATUS "Google Test found - enabling unit tests")
        set(AGENTFLOW_HAS_GTEST ON)
    else()
        pkg_check_modules(GTEST gtest)
        if(GTEST_FOUND)
            message(STATUS "Google Test found (via pkg-config)")
            set(AGENTFLOW_HAS_GTEST ON)
        else()
            message(STATUS "Google Test not found - tests disabled")
            set(AGENTFLOW_HAS_GTEST OFF)
        endif()
    endif()
endif()

# ==============================================================================
# Summary
# ==============================================================================

message(STATUS "")
message(STATUS "=== AgentFlow Dependencies Summary ===")
message(STATUS "Protoactor:   ${AGENTFLOW_HAS_PROTOACTOR}")
message(STATUS "OpenSSL:      ${AGENTFLOW_HAS_OPENSSL}")
message(STATUS "libwebsockets:${AGENTFLOW_HAS_LIBWEBSOCKETS}")
message(STATUS "Protobuf:     ${AGENTFLOW_HAS_PROTOBUF}")
message(STATUS "RapidJSON:    ${AGENTFLOW_HAS_RAPIDJSON}")
message(STATUS "libevent:     ${AGENTFLOW_HAS_LIBEVENT}")
message(STATUS "Google Test:  ${AGENTFLOW_HAS_GTEST}")
message(STATUS "=====================================")
message(STATUS "")
