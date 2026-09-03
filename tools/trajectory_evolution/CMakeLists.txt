cmake_minimum_required(VERSION 3.16)

project(agentflow_trajectory_evolution LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
  find_package(Protobuf REQUIRED)
endif()
find_package(Threads REQUIRED)

if(TARGET protobuf::protoc)
  set(AGENTFLOW_PROTOC_EXECUTABLE "$<TARGET_FILE:protobuf::protoc>")
else()
  set(AGENTFLOW_PROTOC_EXECUTABLE "${Protobuf_PROTOC_EXECUTABLE}")
endif()

find_path(RAPIDJSON_INCLUDE_ROOT
  NAMES rapidjson/document.h
  HINTS
    ${RAPIDJSON_INCLUDE_DIR}
    $ENV{CONDA_PREFIX}
  PATH_SUFFIXES include
)

if(NOT RAPIDJSON_INCLUDE_ROOT)
  message(FATAL_ERROR
    "RapidJSON was not found. Activate the AgentFlow development environment "
    "or pass -DRAPIDJSON_INCLUDE_DIR=<prefix>.")
endif()

set(HYBRID_EVALUATION_PROTO
  ${CMAKE_CURRENT_SOURCE_DIR}/proto/hybrid_evaluation.proto
)
set(HYBRID_EVALUATION_GENERATED_DIR
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)
file(MAKE_DIRECTORY ${HYBRID_EVALUATION_GENERATED_DIR})

add_custom_command(
  OUTPUT
    ${HYBRID_EVALUATION_GENERATED_DIR}/hybrid_evaluation.pb.cc
    ${HYBRID_EVALUATION_GENERATED_DIR}/hybrid_evaluation.pb.h
  COMMAND
    ${AGENTFLOW_PROTOC_EXECUTABLE}
    --cpp_out=${HYBRID_EVALUATION_GENERATED_DIR}
    --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
    ${HYBRID_EVALUATION_PROTO}
  DEPENDS ${HYBRID_EVALUATION_PROTO}
  VERBATIM
)

add_library(agentflow_hybrid_evaluation_proto STATIC
  ${HYBRID_EVALUATION_GENERATED_DIR}/hybrid_evaluation.pb.cc
)
target_include_directories(agentflow_hybrid_evaluation_proto PUBLIC
  ${HYBRID_EVALUATION_GENERATED_DIR}
)
target_link_libraries(agentflow_hybrid_evaluation_proto PUBLIC
  protobuf::libprotobuf
)

add_library(agentflow_trajectory_evolution STATIC
  src/evolution_pipeline.cpp
  src/hybrid_evaluator.cpp
  src/loop_episode_analyzer.cpp
  src/model_adapter.cpp
  src/sessionlog_evaluator.cpp
)
target_include_directories(agentflow_trajectory_evolution
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${RAPIDJSON_INCLUDE_ROOT}
)
target_link_libraries(agentflow_trajectory_evolution PUBLIC
  agentflow_hybrid_evaluation_proto
  CURL::libcurl
  OpenSSL::Crypto
  Threads::Threads
)

if(NOT MSVC)
  target_compile_options(agentflow_trajectory_evolution PRIVATE
    -Wall
    -Wextra
    -Wpedantic
  )
endif()

add_executable(trajectory_evolution
  src/main.cpp
)
target_include_directories(trajectory_evolution PRIVATE
  ${RAPIDJSON_INCLUDE_ROOT}
)
target_link_libraries(trajectory_evolution PRIVATE
  agentflow_trajectory_evolution
)

include(CTest)
if(BUILD_TESTING)
  add_executable(test_trajectory_hybrid_evaluator
    tests/test_hybrid_evaluator.cpp
  )
  target_include_directories(test_trajectory_hybrid_evaluator PRIVATE
    ${RAPIDJSON_INCLUDE_ROOT}
  )
  target_link_libraries(test_trajectory_hybrid_evaluator PRIVATE
    agentflow_trajectory_evolution
  )
  add_test(
    NAME TrajectoryHybridEvaluatorTests
    COMMAND test_trajectory_hybrid_evaluator
  )

  add_executable(test_sessionlog_hybrid_evaluator
    tests/test_sessionlog_evaluator.cpp
  )
  target_include_directories(test_sessionlog_hybrid_evaluator PRIVATE
    ${RAPIDJSON_INCLUDE_ROOT}
  )
  target_link_libraries(test_sessionlog_hybrid_evaluator PRIVATE
    agentflow_trajectory_evolution
  )
  add_test(
    NAME SessionLogHybridEvaluatorTests
    COMMAND test_sessionlog_hybrid_evaluator
  )

  add_executable(test_trajectory_loop_analyzer
    tests/test_loop_episode_analyzer.cpp
  )
  target_include_directories(test_trajectory_loop_analyzer PRIVATE
    ${RAPIDJSON_INCLUDE_ROOT}
  )
  target_link_libraries(test_trajectory_loop_analyzer PRIVATE
    agentflow_trajectory_evolution
  )
  add_test(
    NAME TrajectoryLoopAnalyzerTests
    COMMAND test_trajectory_loop_analyzer
  )

  add_executable(test_trajectory_evolution
    tests/test_evolution_pipeline.cpp
  )
  target_include_directories(test_trajectory_evolution PRIVATE
    ${RAPIDJSON_INCLUDE_ROOT}
  )
  target_link_libraries(test_trajectory_evolution PRIVATE
    agentflow_trajectory_evolution
  )
  add_test(
    NAME TrajectoryEvolutionTests
    COMMAND test_trajectory_evolution
  )
endif()
