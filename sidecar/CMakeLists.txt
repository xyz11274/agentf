cmake_minimum_required(VERSION 3.13)
project(coremind_bridge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(COREMIND_ENABLED "Link the Huawei CoreMind service discovery runtime" ON)

# ── Platform dir (CoreMind .so + headers) ──
# Pass via -DPLATFORM_DIR=/path/to/build/platform
# or default to ../build/platform relative to HealthAgent root
if(NOT PLATFORM_DIR)
    set(PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/platform")
endif()

message(STATUS "coremind_bridge COREMIND_ENABLED=${COREMIND_ENABLED}")
message(STATUS "coremind_bridge PLATFORM_DIR=${PLATFORM_DIR}")

add_executable(coremind_bridge
    main.cpp
    uds_model_proxy.cpp
)

if(COREMIND_ENABLED)
    target_sources(coremind_bridge PRIVATE
        resolve_handler.cpp
        sidecar_router.cpp
    )
else()
    target_sources(coremind_bridge PRIVATE
        resolve_handler_stub.cpp
    )
endif()

# ── Include paths ──
target_include_directories(coremind_bridge PRIVATE
    ${PLATFORM_DIR}/include
    ${PLATFORM_DIR}/include/framework_om
    ${PLATFORM_DIR}/include/framework_common
    ${PLATFORM_DIR}/include/framework_com
    ${PLATFORM_DIR}/include/framework_starter
    ${PLATFORM_DIR}/include/httplib
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Prefer system packages for local development. Huawei builds can continue to
# provide both headers from PLATFORM_DIR.
if(COREMIND_ENABLED)
    find_package(httplib CONFIG QUIET)
else()
    find_package(httplib CONFIG REQUIRED)
endif()
if(httplib_FOUND)
    target_link_libraries(coremind_bridge PRIVATE httplib::httplib)
endif()

if(COREMIND_ENABLED)
    find_package(jsoncpp CONFIG QUIET)
else()
    find_package(jsoncpp CONFIG REQUIRED)
endif()
if(jsoncpp_FOUND)
    target_link_libraries(coremind_bridge PRIVATE JsonCpp::JsonCpp)
else()
    target_link_libraries(coremind_bridge PRIVATE jsoncpp)
endif()

if(COREMIND_ENABLED)
    target_link_directories(coremind_bridge PRIVATE ${PLATFORM_DIR}/lib)
    target_link_libraries(coremind_bridge PRIVATE
        cm_framework_om
        cm_framework_common
        cm_framework_com
        cm_framework_starter
    )
endif()

find_package(Threads REQUIRED)
target_link_libraries(coremind_bridge PRIVATE Threads::Threads)
