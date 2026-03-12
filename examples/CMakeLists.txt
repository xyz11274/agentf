# Copyright (c) 2026 AgentFlow Authors
# Licensed under MIT License

add_library(agentflow_examples_interactive_runner_obj OBJECT
  ${CMAKE_CURRENT_SOURCE_DIR}/features/interactive_runner.cpp
)
target_link_libraries(agentflow_examples_interactive_runner_obj PUBLIC agentflow)
set_target_properties(agentflow_examples_interactive_runner_obj PROPERTIES
  POSITION_INDEPENDENT_CODE ON)
