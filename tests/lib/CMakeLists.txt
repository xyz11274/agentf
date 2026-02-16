cmake_minimum_required(VERSION 3.10)
project(hot_reload_test_lib)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build the test SO library
add_library(hot_reload_test SHARED hot_reload_test.cpp)

# Set output name
set_target_properties(hot_reload_test PROPERTIES
    OUTPUT_NAME "hot_reload_test"
    PREFIX ""
    VERSION 1.0
    SOVERSION 1
)

# On macOS, use .dylib extension
if(APPLE)
    set_target_properties(hot_reload_test PROPERTIES
        SUFFIX ".dylib"
    )
endif()

# Platform-specific settings
if(WIN32)
    target_compile_definitions(hot_reload_test PRIVATE BUILD_DLL)
else()
    target_compile_options(hot_reload_test PRIVATE -fPIC)
endif()
