cmake_minimum_required(VERSION 3.16)

project(agentflow_trajectory_evolution VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

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
find_package(PNG QUIET)

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
    "RapidJSON was not found. Install its development headers "
    "or pass -DRAPIDJSON_INCLUDE_DIR=<prefix>.")
endif()

# 内部实现按职责分目录；这里集中维护搜索路径，避免每个目标重复一长串目录。
set(TRAJECTORY_EVOLUTION_INTERNAL_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/analysis
  ${CMAKE_CURRENT_SOURCE_DIR}/src/runtime
  ${CMAKE_CURRENT_SOURCE_DIR}/src/service
  ${CMAKE_CURRENT_SOURCE_DIR}/src/model
  ${CMAKE_CURRENT_SOURCE_DIR}/src/memory
  ${CMAKE_CURRENT_SOURCE_DIR}/src/evaluation
  ${CMAKE_CURRENT_SOURCE_DIR}/src/gepa
  ${CMAKE_CURRENT_SOURCE_DIR}/src/cli
)

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

set(TRACE_PROTO_IMPORT_DIRS)
get_target_property(TRACE_PROTO_TARGET_INCLUDES protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
foreach(import_dir IN LISTS Protobuf_INCLUDE_DIRS TRACE_PROTO_TARGET_INCLUDES)
  if(IS_DIRECTORY "${import_dir}")
    list(APPEND TRACE_PROTO_IMPORT_DIRS --proto_path=${import_dir})
  endif()
endforeach()
list(REMOVE_DUPLICATES TRACE_PROTO_IMPORT_DIRS)
add_custom_command(
  OUTPUT
    ${HYBRID_EVALUATION_GENERATED_DIR}/trajectory_trace.pb.cc
    ${HYBRID_EVALUATION_GENERATED_DIR}/trajectory_trace.pb.h
  COMMAND ${AGENTFLOW_PROTOC_EXECUTABLE}
    --cpp_out=${HYBRID_EVALUATION_GENERATED_DIR}
    --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
    ${TRACE_PROTO_IMPORT_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/trajectory_trace.proto
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/proto/trajectory_trace.proto
  VERBATIM
)

add_custom_command(
  OUTPUT
    ${HYBRID_EVALUATION_GENERATED_DIR}/shadow_policy.pb.cc
    ${HYBRID_EVALUATION_GENERATED_DIR}/shadow_policy.pb.h
  COMMAND ${AGENTFLOW_PROTOC_EXECUTABLE}
    --cpp_out=${HYBRID_EVALUATION_GENERATED_DIR}
    --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
    ${TRACE_PROTO_IMPORT_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/shadow_policy.proto
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/proto/shadow_policy.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/trajectory_trace.proto
  VERBATIM
)

add_custom_command(
  OUTPUT ${HYBRID_EVALUATION_GENERATED_DIR}/runtime_policy.pb.cc
    ${HYBRID_EVALUATION_GENERATED_DIR}/runtime_policy.pb.h
  COMMAND ${AGENTFLOW_PROTOC_EXECUTABLE}
    --cpp_out=${HYBRID_EVALUATION_GENERATED_DIR}
    --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
    ${TRACE_PROTO_IMPORT_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/runtime_policy.proto
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/proto/runtime_policy.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/shadow_policy.proto
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/trajectory_trace.proto
  VERBATIM
)

add_custom_command(
  OUTPUT ${HYBRID_EVALUATION_GENERATED_DIR}/evaluation_dataset.pb.cc
    ${HYBRID_EVALUATION_GENERATED_DIR}/evaluation_dataset.pb.h
  COMMAND ${AGENTFLOW_PROTOC_EXECUTABLE}
    --cpp_out=${HYBRID_EVALUATION_GENERATED_DIR}
    --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
    ${TRACE_PROTO_IMPORT_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/proto/evaluation_dataset.proto
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/proto/evaluation_dataset.proto
  VERBATIM
)

add_library(agentflow_hybrid_evaluation_proto STATIC
  ${HYBRID_EVALUATION_GENERATED_DIR}/hybrid_evaluation.pb.cc
  ${HYBRID_EVALUATION_GENERATED_DIR}/trajectory_trace.pb.cc
  ${HYBRID_EVALUATION_GENERATED_DIR}/shadow_policy.pb.cc
  ${HYBRID_EVALUATION_GENERATED_DIR}/runtime_policy.pb.cc
  ${HYBRID_EVALUATION_GENERATED_DIR}/evaluation_dataset.pb.cc
)
set_target_properties(agentflow_hybrid_evaluation_proto PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(agentflow_hybrid_evaluation_proto PUBLIC
  $<BUILD_INTERFACE:${HYBRID_EVALUATION_GENERATED_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(agentflow_hybrid_evaluation_proto PUBLIC
  protobuf::libprotobuf
)

add_library(agentflow_trajectory_evolution STATIC
  src/evaluation/agent_evaluation.cpp
  src/evaluation/dataset.cpp
  src/evaluation/task_evaluation.cpp
  src/evaluation/evaluation_process.cpp
  src/analysis/evolution_pipeline.cpp
  src/analysis/hybrid_evaluator.cpp
  src/analysis/loop_episode_analyzer.cpp
  src/model/model_adapter.cpp
  src/model/runtime_config.cpp
  src/analysis/sessionlog_evaluator.cpp
  src/analysis/trace_contract.cpp
  src/analysis/trace_analyzer.cpp
  src/runtime/shadow_runtime.cpp
  src/memory/shared_memory.cpp
  src/runtime/policy_runtime.cpp
  src/runtime/orchestration.cpp
  src/runtime/request_binding.cpp
  src/service/integration_service.cpp
  src/service/integration_http.cpp
  src/runtime/boundary_runtime.cpp
  src/model/model_contract.cpp
  src/runtime/instrumentation.cpp
  src/runtime/dispatch_runtime.cpp
  src/gepa/control_generation.cpp
  src/gepa/gepa_search.cpp
  src/gepa/gepa_strategy.cpp
  src/service/local_runtime.cpp
)
set_target_properties(agentflow_trajectory_evolution PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(agentflow_trajectory_evolution
  PRIVATE ${TRAJECTORY_EVOLUTION_INTERNAL_INCLUDE_DIRS}
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${RAPIDJSON_INCLUDE_ROOT}
)
target_link_libraries(agentflow_trajectory_evolution PUBLIC
  agentflow_hybrid_evaluation_proto
  CURL::libcurl
  OpenSSL::Crypto
  Threads::Threads
  ${CMAKE_DL_LIBS}
)
if(PNG_FOUND)
  target_link_libraries(agentflow_trajectory_evolution PUBLIC PNG::PNG)
  target_compile_definitions(agentflow_trajectory_evolution PRIVATE EVOLUTION_HAS_PNG=1)
endif()
set_source_files_properties(src/evaluation/evaluation_process.cpp PROPERTIES COMPILE_DEFINITIONS
  "EVOLUTION_BUILD_CLI=\"${CMAKE_CURRENT_BINARY_DIR}/trajectory_evolution\";EVOLUTION_INSTALLED_CLI=\"${CMAKE_INSTALL_FULL_BINDIR}/trajectory_evolution\"")

if(NOT MSVC)
  target_compile_options(agentflow_trajectory_evolution PRIVATE
    -Wall
    -Wextra
    -Wpedantic
  )
endif()

add_executable(trajectory_evolution
  src/cli/evaluation_supervisor.cpp
  src/cli/learn_cli.cpp
  src/cli/dataset_cli.cpp
  src/cli/task_evaluation_cli.cpp
  src/cli/main.cpp
  src/cli/trace_cli.cpp
  src/cli/shadow_cli.cpp
  src/cli/policy_cli.cpp
)
target_include_directories(trajectory_evolution PRIVATE
  ${TRAJECTORY_EVOLUTION_INTERNAL_INCLUDE_DIRS}
  ${RAPIDJSON_INCLUDE_ROOT}
)
target_link_libraries(trajectory_evolution PRIVATE
  agentflow_trajectory_evolution
)

set_target_properties(agentflow_trajectory_evolution PROPERTIES EXPORT_NAME core)
set_target_properties(agentflow_hybrid_evaluation_proto PROPERTIES EXPORT_NAME proto)
add_library(TrajectoryEvolution::core ALIAS agentflow_trajectory_evolution)
target_sources(agentflow_trajectory_evolution PRIVATE clients/c/evolution.c)
add_library(trajectory_evolution_local SHARED src/service/local_c.cpp src/service/evaluation_entry_client.cpp
    clients/cpp/evolution_points.cpp)
target_include_directories(trajectory_evolution_local PRIVATE ${TRAJECTORY_EVOLUTION_INTERNAL_INCLUDE_DIRS} ${RAPIDJSON_INCLUDE_ROOT})
target_include_directories(trajectory_evolution_local PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(trajectory_evolution_local PRIVATE agentflow_trajectory_evolution)
set_target_properties(trajectory_evolution_local PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 1
    EXPORT_NAME local INSTALL_RPATH "$ORIGIN")
install(TARGETS trajectory_evolution_local EXPORT TrajectoryEvolutionTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
find_program(LOCAL_NODE_EXECUTABLE node)
if(LOCAL_NODE_EXECUTABLE)
  get_filename_component(LOCAL_NODE_BIN "${LOCAL_NODE_EXECUTABLE}" DIRECTORY)
  find_path(LOCAL_NODE_INCLUDE node_api.h HINTS "${LOCAL_NODE_BIN}/../include/node"
      /usr/include/node /usr/local/include/node)
  if(LOCAL_NODE_INCLUDE)
    add_library(trajectory_evolution_local_node MODULE src/service/local_node.cpp)
    target_include_directories(trajectory_evolution_local_node PRIVATE include "${LOCAL_NODE_INCLUDE}")
    target_link_libraries(trajectory_evolution_local_node PRIVATE trajectory_evolution_local)
    set_target_properties(trajectory_evolution_local_node PROPERTIES PREFIX "" SUFFIX ".node" INSTALL_RPATH "$ORIGIN")
    install(TARGETS trajectory_evolution_local_node LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
  endif()
endif()
install(TARGETS agentflow_trajectory_evolution agentflow_hybrid_evaluation_proto
    trajectory_evolution EXPORT TrajectoryEvolutionTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${HYBRID_EVALUATION_GENERATED_DIR}/hybrid_evaluation.pb.h
    ${HYBRID_EVALUATION_GENERATED_DIR}/trajectory_trace.pb.h
    ${HYBRID_EVALUATION_GENERATED_DIR}/shadow_policy.pb.h
    ${HYBRID_EVALUATION_GENERATED_DIR}/runtime_policy.pb.h
    ${HYBRID_EVALUATION_GENERATED_DIR}/evaluation_dataset.pb.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY config/ DESTINATION ${CMAKE_INSTALL_DATADIR}/trajectory_evolution/config)
install(DIRECTORY proto/ DESTINATION ${CMAKE_INSTALL_DATADIR}/trajectory_evolution/proto)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples")
  install(DIRECTORY examples/ DESTINATION ${CMAKE_INSTALL_DATADIR}/trajectory_evolution/examples
      PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE)
endif()
install(DIRECTORY integrations/ DESTINATION ${CMAKE_INSTALL_DATADIR}/trajectory_evolution/integrations
    PATTERN "__pycache__" EXCLUDE PATTERN "*.pyc" EXCLUDE)
install(DIRECTORY clients/ DESTINATION ${CMAKE_INSTALL_DATADIR}/trajectory_evolution/clients
    PATTERN "target" EXCLUDE PATTERN "node_modules" EXCLUDE PATTERN "__pycache__" EXCLUDE
    PATTERN "*.pyc" EXCLUDE PATTERN "bin" EXCLUDE PATTERN "obj" EXCLUDE PATTERN "*.egg-info" EXCLUDE)
install(DIRECTORY api/ DESTINATION ${CMAKE_INSTALL_DATADIR}/trajectory_evolution/api)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/trajectory_evolution/docs)
install(FILES README.md ALGORITHMS.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/trajectory_evolution)
configure_package_config_file(cmake/TrajectoryEvolutionConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/TrajectoryEvolutionConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TrajectoryEvolution)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/TrajectoryEvolutionConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/TrajectoryEvolutionConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/TrajectoryEvolutionConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TrajectoryEvolution)
install(EXPORT TrajectoryEvolutionTargets NAMESPACE TrajectoryEvolution::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TrajectoryEvolution)

include(CTest)
if(BUILD_TESTING)
  add_executable(test_local_runtime tests/test_local_runtime.cpp)
  target_include_directories(test_local_runtime PRIVATE ${TRAJECTORY_EVOLUTION_INTERNAL_INCLUDE_DIRS} ${RAPIDJSON_INCLUDE_ROOT})
  target_compile_definitions(test_local_runtime PRIVATE LOCAL_TEST_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}"
      LOCAL_TEST_BUILD="${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(test_local_runtime PRIVATE agentflow_trajectory_evolution)
  add_test(NAME LocalRuntimeTests COMMAND test_local_runtime)
  set_tests_properties(LocalRuntimeTests PROPERTIES TIMEOUT 120)
  add_library(trajectory_http_client_test STATIC clients/c/evolution.c)
  target_link_libraries(trajectory_http_client_test PUBLIC CURL::libcurl)
  add_executable(test_client_c tests/clients/probe.c)
  target_link_libraries(test_client_c PRIVATE trajectory_http_client_test)
  add_executable(test_client_cpp tests/clients/probe.cpp)
  target_link_libraries(test_client_cpp PRIVATE trajectory_http_client_test)
  add_executable(test_evolution_points clients/cpp/evolution_points_test.cpp)
  target_include_directories(test_evolution_points PRIVATE ${RAPIDJSON_INCLUDE_ROOT})
  target_link_libraries(test_evolution_points PRIVATE trajectory_evolution_local)
  add_test(NAME EvolutionPointsTests COMMAND test_evolution_points)
  find_program(TRAJECTORY_EVOLUTION_GO_EXECUTABLE go)
  if(TRAJECTORY_EVOLUTION_GO_EXECUTABLE)
    add_test(NAME GoLocalClientTests
        COMMAND ${TRAJECTORY_EVOLUTION_GO_EXECUTABLE} test -race ./...)
    set_tests_properties(GoLocalClientTests PROPERTIES TIMEOUT 120
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/clients/go
        ENVIRONMENT
            "TRAJECTORY_EVOLUTION_TEST_BUILD=${CMAKE_CURRENT_BINARY_DIR}")
  endif()
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME DispatchClientTests COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dispatch_client.py ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(DispatchClientTests PROPERTIES TIMEOUT 120)
    add_test(NAME AgentEvaluationTests COMMAND ${Python3_EXECUTABLE}
        -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_agent_evaluation.py ${CMAKE_CURRENT_BINARY_DIR} ${AGENTFLOW_PROTOC_EXECUTABLE})
    set_tests_properties(AgentEvaluationTests PROPERTIES TIMEOUT 240)
    add_test(NAME TrainingBootstrapTests COMMAND ${Python3_EXECUTABLE}
        -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_training_bootstrap.py ${CMAKE_CURRENT_BINARY_DIR} ${AGENTFLOW_PROTOC_EXECUTABLE})
    set_tests_properties(TrainingBootstrapTests PROPERTIES TIMEOUT 180)
    add_test(NAME GepaSearchTests COMMAND ${Python3_EXECUTABLE}
        -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_gepa_search.py ${CMAKE_CURRENT_BINARY_DIR} ${AGENTFLOW_PROTOC_EXECUTABLE})
    # 搜索回归包含多组串行场景；上限覆盖慢机器，但不改变任何运行时超时。
    set_tests_properties(GepaSearchTests PROPERTIES TIMEOUT 1800)
    add_test(NAME DocumentationTests COMMAND ${Python3_EXECUTABLE}
        -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_documentation.py)
    add_test(NAME TaskEvaluationTests COMMAND ${Python3_EXECUTABLE}
        -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_task_evaluation.py ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(TaskEvaluationTests PROPERTIES TIMEOUT 180)
    add_test(NAME EvaluationDatasetTests COMMAND ${Python3_EXECUTABLE}
        -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dataset.py ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(EvaluationDatasetTests PROPERTIES TIMEOUT 180)
    add_test(NAME LearningScheduleTests COMMAND ${Python3_EXECUTABLE}
        -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_learning_schedule.py ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(LearningScheduleTests PROPERTIES TIMEOUT 120)
    add_test(NAME UnifiedLearningTests COMMAND ${Python3_EXECUTABLE}
        -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_unified_learning.py ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(UnifiedLearningTests PROPERTIES TIMEOUT 120)
    add_test(NAME LocalLanguageClientTests COMMAND ${Python3_EXECUTABLE}
        -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_local_clients.py ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(LocalLanguageClientTests PROPERTIES TIMEOUT 120)
    add_test(NAME IntegrationLanguageClientTests COMMAND ${Python3_EXECUTABLE}
        -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_language_clients.py ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(IntegrationLanguageClientTests PROPERTIES TIMEOUT 180)
    add_test(NAME IntegrationOpenApiContractTests COMMAND ${Python3_EXECUTABLE}
        -B ${CMAKE_CURRENT_SOURCE_DIR}/api/build_openapi.py --check)
    add_test(NAME IntegrationDistributedTests COMMAND ${Python3_EXECUTABLE}
        -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_distributed_integration.py ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(IntegrationDistributedTests PROPERTIES TIMEOUT 120)
    add_test(NAME BoundaryTransportTests COMMAND ${Python3_EXECUTABLE}
        -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_boundary_transport.py ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(BoundaryTransportTests PROPERTIES TIMEOUT 120)
    add_test(NAME InstrumentationTests COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_instrumentation.py ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(InstrumentationTests PROPERTIES TIMEOUT 120)
    add_test(NAME ConfiguredPointClientTests COMMAND ${Python3_EXECUTABLE}
      -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_configured_points.py ${CMAKE_CURRENT_BINARY_DIR})
    add_test(NAME AutomaticContractTests COMMAND ${Python3_EXECUTABLE}
      -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_automatic_contracts.py ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(AutomaticContractTests PROPERTIES TIMEOUT 120)
  endif()
  add_executable(test_integration_service tests/test_integration_service.cpp)
  target_include_directories(test_integration_service PRIVATE ${TRAJECTORY_EVOLUTION_INTERNAL_INCLUDE_DIRS} ${RAPIDJSON_INCLUDE_ROOT})
  target_compile_definitions(test_integration_service PRIVATE
      INTEGRATION_TEST_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(test_integration_service PRIVATE agentflow_trajectory_evolution)
  add_test(NAME IntegrationServiceTests COMMAND test_integration_service)
  find_program(INTEGRATION_NODE_EXECUTABLE node)
  if(INTEGRATION_NODE_EXECUTABLE)
    execute_process(COMMAND ${INTEGRATION_NODE_EXECUTABLE} --version
        OUTPUT_VARIABLE INTEGRATION_NODE_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REGEX REPLACE "^v" "" INTEGRATION_NODE_VERSION "${INTEGRATION_NODE_VERSION}")
    if(INTEGRATION_NODE_VERSION VERSION_GREATER_EQUAL "22.18")
      add_test(NAME IntegrationHttpTests COMMAND ${INTEGRATION_NODE_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_integration_http.ts ${CMAKE_CURRENT_BINARY_DIR})
      set_tests_properties(IntegrationHttpTests PROPERTIES TIMEOUT 120)
    endif()
  endif()
  add_executable(test_policy_runtime tests/test_policy_runtime.cpp)
  target_include_directories(test_policy_runtime PRIVATE ${TRAJECTORY_EVOLUTION_INTERNAL_INCLUDE_DIRS})
  add_executable(test_gepa_config tests/test_gepa_config.cpp)
  target_include_directories(test_gepa_config PRIVATE ${TRAJECTORY_EVOLUTION_INTERNAL_INCLUDE_DIRS} ${RAPIDJSON_INCLUDE_ROOT})
  target_link_libraries(test_gepa_config PRIVATE agentflow_trajectory_evolution)
  add_test(NAME GepaConfigurationTests COMMAND test_gepa_config ${CMAKE_CURRENT_SOURCE_DIR}/config/gepa.json)
  add_executable(test_gepa_strategy tests/test_gepa_strategy.cpp)
  target_include_directories(test_gepa_strategy PRIVATE ${TRAJECTORY_EVOLUTION_INTERNAL_INCLUDE_DIRS} ${RAPIDJSON_INCLUDE_ROOT})
  target_link_libraries(test_gepa_strategy PRIVATE agentflow_trajectory_evolution)
  add_test(NAME GepaStrategyTests COMMAND test_gepa_strategy)
  add_executable(test_gepa_evidence tests/test_gepa_evidence.cpp)
  target_include_directories(test_gepa_evidence PRIVATE ${TRAJECTORY_EVOLUTION_INTERNAL_INCLUDE_DIRS} ${RAPIDJSON_INCLUDE_ROOT})
  target_link_libraries(test_gepa_evidence PRIVATE agentflow_trajectory_evolution)
  add_test(NAME GepaEvidenceTests COMMAND test_gepa_evidence)
  add_executable(test_shared_memory tests/test_shared_memory.cpp)
  target_include_directories(test_shared_memory PRIVATE ${TRAJECTORY_EVOLUTION_INTERNAL_INCLUDE_DIRS} ${RAPIDJSON_INCLUDE_ROOT})
  target_link_libraries(test_shared_memory PRIVATE agentflow_trajectory_evolution)
  add_test(NAME SharedMemoryTests COMMAND test_shared_memory)
  target_link_libraries(test_policy_runtime PRIVATE agentflow_trajectory_evolution)
  add_test(NAME PolicyRuntimeTests COMMAND test_policy_runtime)
  add_executable(test_shadow_runtime tests/test_shadow_runtime.cpp)
  target_link_libraries(test_shadow_runtime PRIVATE agentflow_trajectory_evolution)
  add_test(NAME ShadowRuntimeTests COMMAND test_shadow_runtime)

  add_executable(test_standard_trace tests/test_standard_trace.cpp)
  target_include_directories(test_standard_trace PRIVATE ${RAPIDJSON_INCLUDE_ROOT})
  target_compile_definitions(test_standard_trace PRIVATE
      TRACE_TEST_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(test_standard_trace PRIVATE agentflow_trajectory_evolution)
  add_test(NAME StandardTrajectoryTests COMMAND test_standard_trace)
  add_test(NAME StandardTrajectoryCliTests COMMAND ${CMAKE_COMMAND}
      -DEVOLUTION_EXE=$<TARGET_FILE:trajectory_evolution>
      -DTRACE_INPUT=${CMAKE_CURRENT_SOURCE_DIR}/examples/standard-agent/one.trace.json
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_trace_cli.cmake)

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

  add_executable(test_trajectory_runtime_config
    tests/test_runtime_config.cpp
  )
  target_include_directories(test_trajectory_runtime_config PRIVATE
    ${RAPIDJSON_INCLUDE_ROOT}
  )
  target_link_libraries(test_trajectory_runtime_config PRIVATE
    agentflow_trajectory_evolution
  )
  add_test(
    NAME TrajectoryRuntimeConfigTests
    COMMAND test_trajectory_runtime_config
  )
endif()
