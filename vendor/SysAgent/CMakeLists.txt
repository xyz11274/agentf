set(LIB "SysAgent")

message("------------ SysAgent src begin -------------")
set(CMAKE_CXX_FLAGS "-fPIC -Wall ${CMAKE_CXX_FLAGS}")
set(CMAKE_CXX_FLAGS_DEBUG "-O2 ${CMAKE_CXX_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 ${CMAKE_CXX_FLAGS}")

file(GLOB_RECURSE SRC_FILELIST "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE SRC_INCLUDE_INNER_FILELIST "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
file(GLOB_RECURSE SRC_INCLUDE_OUTER_FILELIST "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")

file(GLOB_RECURSE SRC_FILES
    ${SRC_INCLUDE_OUTER_FILELIST}
    ${SRC_INCLUDE_INNER_FILELIST}
    ${SRC_FILELIST}
)

add_executable(${LIB} ${SRC_FILES})

target_link_directories(
        ${LIB}
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/lib # 第三方库文件依赖
)

target_include_directories(
        ${LIB}
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include/
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include/message/
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include/taskManage/
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include/registerManage/
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/llm_plugin
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/tool_plugin
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/rag_plugin
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/mmagentflow
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/cssp
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/framework_com
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/framework_common
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/framework_common/utils
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/framework_om
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/framework_security
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/framework_starter
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/rag/embedding
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/rag/datasource
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/rag/retriever
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/rag/rerank
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/libevent
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/agentflow
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/agentflow/internal
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../platform/include/agentflow/external
        # SKILL
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../Skill/com/include/external
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../Skill/common/include/external
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../Skill/plugin/include/external
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../Skill/runtime/include/external
)

target_link_libraries(
        ${LIB}
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
        PUBLIC curl
        PUBLIC agentflow
        PUBLIC mmplugins
        PUBLIC toolplugincpp
        PUBLIC contextplugin
        PUBLIC skill_plugin
)

install(PROGRAMS ${LIB} DESTINATION .)