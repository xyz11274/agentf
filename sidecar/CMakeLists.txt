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
    proxy_handler.cpp
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
    ${PLATFORM_DIR}/include/mmagentflow
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
        matemindsdk
    )
endif()

find_package(Threads REQUIRED)
target_link_libraries(coremind_bridge PRIVATE Threads::Threads)

# ═══════════════════════════════════════════════════════════
# Unit tests — pure logic, no CMF dependency
# Skipped silently when GTest is unavailable (e.g. production CI).
# ═══════════════════════════════════════════════════════════

find_package(GTest QUIET)
if(GTest_FOUND)
    enable_testing()

    add_executable(test_router_logic
        tests/test_router_logic.cpp
        router_logic.cpp
    )

    target_include_directories(test_router_logic PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(test_router_logic PRIVATE GTest::GTest GTest::Main)
    # Ensure runtime can find GTest dylib (e.g. miniconda install)
    if(GTest_DIR)
        get_filename_component(GTEST_LIB_DIR "${GTest_DIR}/../.." ABSOLUTE)
        set_target_properties(test_router_logic PROPERTIES
            BUILD_RPATH "${GTEST_LIB_DIR}")
    endif()

    if(jsoncpp_FOUND)
        target_link_libraries(test_router_logic PRIVATE JsonCpp::JsonCpp)
    else()
        target_link_libraries(test_router_logic PRIVATE jsoncpp)
    endif()

    include(GoogleTest)
    gtest_discover_tests(test_router_logic)
else()
    message(STATUS "sidecar: GTest not found — skipping unit tests")
endif()
