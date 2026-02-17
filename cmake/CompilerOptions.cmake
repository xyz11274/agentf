# AgentFlow Compiler Options
#
# This module sets up compiler options and flags for the AgentFlow project.

# ==============================================================================
# Common Compiler Flags
# ==============================================================================

# Enable position independent code
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Warning flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")

# Debug flags
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")

# Release flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# RelWithDebInfo flags
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")

# ==============================================================================
# Platform-specific Options
# ==============================================================================

if(APPLE)
    # macOS specific options
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden")
elseif(UNIX)
    # Linux specific options
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden")
endif()

# ==============================================================================
# Sanitizer Options (Debug only)
# ==============================================================================

option(AGENTFLOW_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(AGENTFLOW_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(AGENTFLOW_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(AGENTFLOW_ENABLE_ASAN)
        message(STATUS "AddressSanitizer enabled")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    endif()

    if(AGENTFLOW_ENABLE_TSAN)
        message(STATUS "ThreadSanitizer enabled")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread -fno-omit-frame-pointer")
    endif()

    if(AGENTFLOW_ENABLE_UBSAN)
        message(STATUS "UndefinedBehaviorSanitizer enabled")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=undefined -fno-omit-frame-pointer")
    endif()
endif()

# ==============================================================================
# Coverage Options
# ==============================================================================

option(AGENTFLOW_ENABLE_COVERAGE "Enable code coverage" OFF)

if(AGENTFLOW_ENABLE_COVERAGE)
    message(STATUS "Code coverage enabled")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage -fprofile-arcs -ftest-coverage")
endif()

# ==============================================================================
# Export Compile Commands
# ==============================================================================

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

message(STATUS "Compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "CXX Flags: ${CMAKE_CXX_FLAGS}")
message(STATUS "CXX Flags (Debug): ${CMAKE_CXX_FLAGS_DEBUG}")
message(STATUS "CXX Flags (Release): ${CMAKE_CXX_FLAGS_RELEASE}")
