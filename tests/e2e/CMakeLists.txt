# E2E Tests CMake Configuration

add_custom_target(e2e_tests)

# Message Routing Scenario
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/message_routing/test_message_routing.cpp")
    add_executable(test_e2e_message_routing
        message_routing/test_message_routing.cpp
    )
    target_link_libraries(test_e2e_message_routing
        agentflow
        GTest::gtest
        GTest::gtest_main
    )
    add_test(NAME E2EMessageRouting COMMAND test_e2e_message_routing)
    add_dependencies(e2e_tests test_e2e_message_routing)
endif()

# Workflow Execution Scenario - Basic Agent E2E
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/workflow_execution/test_basic_agent_e2e.cpp")
    add_executable(test_e2e_basic_agent
        workflow_execution/test_basic_agent_e2e.cpp
    )
    target_link_libraries(test_e2e_basic_agent
        agentflow
        GTest::gtest
        GTest::gtest_main
    )
    add_test(NAME E2EBasicAgent COMMAND test_e2e_basic_agent)
    add_dependencies(e2e_tests test_e2e_basic_agent)
endif()

# Session Isolation Scenario
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/session_isolation/test_session_isolation.cpp")
    add_executable(test_e2e_session_isolation
        session_isolation/test_session_isolation.cpp
    )
    target_link_libraries(test_e2e_session_isolation
        agentflow
        GTest::gtest
        GTest::gtest_main
    )
    add_test(NAME E2ESessionIsolation COMMAND test_e2e_session_isolation)
    add_dependencies(e2e_tests test_e2e_session_isolation)
endif()

# Data Sharing Scenario
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/data_sharing/test_data_sharing.cpp")
    add_executable(test_e2e_data_sharing
        data_sharing/test_data_sharing.cpp
    )
    target_link_libraries(test_e2e_data_sharing
        agentflow
        GTest::gtest
        GTest::gtest_main
    )
    add_test(NAME E2EDataSharing COMMAND test_e2e_data_sharing)
    add_dependencies(e2e_tests test_e2e_data_sharing)
endif()

# Nested Workflow Scenario
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/workflow_execution/test_nested_workflow_e2e.cpp")
    add_executable(test_e2e_nested_workflow
        workflow_execution/test_nested_workflow_e2e.cpp
    )
    target_link_libraries(test_e2e_nested_workflow
        agentflow
        GTest::gtest
        GTest::gtest_main
    )
    add_test(NAME E2ENestedWorkflow COMMAND test_e2e_nested_workflow)
    add_dependencies(e2e_tests test_e2e_nested_workflow)
endif()

# Interactive Runner black-box smoke test
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/test_blackbox_interactive_runner.sh")
    add_test(
        NAME E2EInteractiveRunner
        COMMAND bash
                "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/test_blackbox_interactive_runner.sh"
                "${CMAKE_BINARY_DIR}/agent_runner"
                "${CMAKE_SOURCE_DIR}/examples/agent/basic_agent"
    )
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/examples/agent/multi_session_chat/tests/auto_dialog_test.sh")
    add_test(
        NAME E2EMultiSessionChatTUI
        COMMAND bash
                "${CMAKE_SOURCE_DIR}/examples/agent/multi_session_chat/tests/auto_dialog_test.sh"
    )
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/examples/agent/openjiuwen_multi_session_chat/tests/auto_dialog_test.sh")
    add_test(
        NAME E2EOpenJiuwenMultiSessionChat
        COMMAND bash
                "${CMAKE_SOURCE_DIR}/examples/agent/openjiuwen_multi_session_chat/tests/auto_dialog_test.sh"
    )
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/examples/agent/claw_agent/tests/test_claw_agent_tui.sh")
    add_test(
        NAME E2EClawAgentTUI
        COMMAND bash
                "${CMAKE_SOURCE_DIR}/examples/agent/claw_agent/tests/test_claw_agent_tui.sh"
    )
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/examples/agent/openjiuwen_studio_agent/tests/test_openjiuwen_studio_agent_tui.sh")
    add_test(
        NAME E2EOpenJiuwenStudioAgentTUI
        COMMAND bash
                "${CMAKE_SOURCE_DIR}/examples/agent/openjiuwen_studio_agent/tests/test_openjiuwen_studio_agent_tui.sh"
    )
endif()

# Echo Agent Scenario
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/echo_agent/test_echo_agent_e2e.cpp")
    add_executable(test_e2e_echo_agent
        echo_agent/test_echo_agent_e2e.cpp
    )
    target_link_libraries(test_e2e_echo_agent
        agentflow
        GTest::gtest
        GTest::gtest_main
    )
    add_test(NAME E2EEchoAgent COMMAND test_e2e_echo_agent)
    add_dependencies(e2e_tests test_e2e_echo_agent)
endif()

# LLM Query Agent Scenario (black-box test through message interface)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/llm_query_agent/test_llm_query_agent_e2e.cpp")
    add_executable(test_e2e_llm_query_agent
        llm_query_agent/test_llm_query_agent_e2e.cpp
    )
    target_link_libraries(test_e2e_llm_query_agent
        agentflow
        GTest::gtest
        GTest::gtest_main
    )
    add_test(NAME E2ELLMQueryAgent COMMAND test_e2e_llm_query_agent)
    add_dependencies(e2e_tests test_e2e_llm_query_agent)
endif()
