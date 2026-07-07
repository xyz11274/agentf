# Copyright (c) Huawei Technologies Co., Ltd. 2021-2024. All rights reserved.
# Description: CMakeLists.txt
# Create: 2021-02-26

cmake_minimum_required(VERSION 3.13)

project(SystemTest VERSION 1.0 LANGUAGES C CXX)
set(MODULE_NAME SystemTest)

set(COMPILER_EXEC_PATH /opt/buildtools/BiSheng_native/bin)
set(CMAKE_C_COMPILER ${COMPILER_EXEC_PATH}/clang)
set(CMAKE_CXX_COMPILER ${COMPILER_EXEC_PATH}/clang++)
set(GSP_PKG_LINKER ${COMPILER_EXEC_PATH}/ld.lld)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_GLIBCXX_USE_CXX11_ABI=0")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fuse-ld=lld -fcommon -gdwarf-4 -fsigned-char -Wall -g -O0 -fno-strict-aliasing -fno-pie -fPIC -Wl,--allow-shlib-undefined,-z,relro,-z,now,-z,noexecstack -fdiagnostics-color=never -fstack-protector-all")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    # 适配X86跳转岛
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-T${PROJECT_SOURCE_DIR}/build/trampoline.lds")
    # X86毕昇插入nop
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mllvm -x86-enable-insert-nopl=true")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mllvm -x86-enable-insert-nopl=true")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64")
    # 适配ARM跳转岛
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-T${PROJECT_SOURCE_DIR}/../build/ld.lds")
else()
    message(STATUS "Unknown platform: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# 包含所有组件的cmake
set(GLIBCXX_USE_CXX11_ABI_0 ON)
include("${CMAKE_CURRENT_SOURCE_DIR}/conf/comp.cmake")

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include/additional)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)

#find_package(PythonLibs 3.11 REQUIRED)
#include_directories(${PYTHON_INCLUDE_DIRS})

# 添加测试源代码
aux_source_directory(src TEST_SRC)
aux_source_directory(src/sample1 TEST_SRC)

## 业务代码
file(GLOB_RECURSE SRC_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/callBack/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/graphManager/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/memory/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/model/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/queue/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/queue/Lifecycle/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/ruleCognition/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/server/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/taskManage/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/utils/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/message/*.cpp

        ${CMAKE_CURRENT_SOURCE_DIR}/../src/aisf_platform/parser/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/aisf_platform/string/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/aisf_platform/thread/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/aisf_platform/utils/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/aisf_platform/websockets/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

# 生成目标
add_executable(${MODULE_NAME} ${SRC_FILES})
target_link_directories(
        ${MODULE_NAME}
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/lib # 第三方库文件依赖
)

target_compile_options(${MODULE_NAME} PRIVATE -fno-access-control)

target_include_directories(
        ${MODULE_NAME}
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include/
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include/message/
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include/taskManage/
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/include
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/include/agentflow
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/include/cssp
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/include/framework_com
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/include/framework_common
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/include/framework_om
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/include/framework_security
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/include/framework_starter
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/include/rag/embedding
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/include/rag/datasource
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../../platform/include/libevent
)

target_link_libraries(
        ${MODULE_NAME}
        PUBLIC
        PUBLIC securec
        PUBLIC jsoncpp
        PUBLIC event
        PUBLIC event_pthreads
        PUBLIC websockets
        PUBLIC cm_framework_com
        PUBLIC cm_framework_common
        PUBLIC cm_framework_om
        PUBLIC cm_framework_starter
        PUBLIC cm_framework_security
        PUBLIC matemindsdk
        PUBLIC mmagentflow
        PUBLIC RagSdk
        #        ${PYTHON_LIBRARIES}
        gtest_main
)

# set cpu architecture
if(${CPU_ARCHITECTURE} MATCHES "ARM")
    message("*** I am ARM architecture ***")
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_SYSTEM_PROCESSOR arm)
    set(ENV_VOS_OS_VER 4)
    set(ENV_VOS_CPU_TYPE 49)
    set(ENV_VOS_HARDWARE_PLATFORM 8)
    set(ENV_VOS_WORDSIZE 64)
else()
    message("*** I am X86 architecture ***")
    add_compile_options(-m64)
    set(ENV_VOS_OS_VER 4)
    set(ENV_VOS_CPU_TYPE 0)
    set(ENV_VOS_HARDWARE_PLATFORM 0)
    set(ENV_VOS_WORDSIZE 64)
endif()
#包含的预定义宏。
add_definitions(
        -DVOS_OS_VER=${ENV_VOS_OS_VER}
        -DVOS_CPU_TYPE=${ENV_VOS_CPU_TYPE}
        -DVOS_HARDWARE_PLATFORM=${ENV_VOS_HARDWARE_PLATFORM}
        -DVOS_WORDSIZE=${ENV_VOS_WORDSIZE}
        -D_GLIBCXX_USE_CXX11_ABI=0
)

# 打印构建选项
get_target_property(COMPILE_FLAGS ${MODULE_NAME} COMPILE_OPTIONS)
get_target_property(LINK_FLAGS ${MODULE_NAME} LINK_OPTIONS)
message(STATUS "Compiler c version: ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "Compiler cxx version: ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Compiler c std version: ${CMAKE_C_STANDARD}")
message(STATUS "Compiler cxx std version: ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler id: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "Compile flags: ${COMPILE_FLAGS}")
message(STATUS "Link flags: ${LINK_FLAGS}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
