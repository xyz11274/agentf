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

# Workflow Execution Scenario — REMOVED: test_basic_agent_e2e.cpp and test_nested_workflow_e2e.cpp
# were deleted along with their test data (basic_agent, nested_workflow_agent) in commit 61fb5cb.

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

# DataBus Cleanup Regression Scenario
# 锁住漏洞 ①-⑧：老化清理、session_destroy 双边清、unregister 容量释放、
# store_with_ref 索引叠加、trace span 全丢、TraceContext 跨请求串扰
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/databus_cleanup/test_databus_cleanup_e2e.cpp")
    add_executable(test_e2e_databus_cleanup
        databus_cleanup/test_databus_cleanup_e2e.cpp
    )
    target_link_libraries(test_e2e_databus_cleanup
        agentflow
        GTest::gtest
        GTest::gtest_main
    )
    add_test(NAME E2EDataBusCleanup COMMAND test_e2e_databus_cleanup)
    add_dependencies(e2e_tests test_e2e_databus_cleanup)
endif()

# Nested Workflow Scenario — REMOVED: test data (nested_workflow_agent) deleted in commit 61fb5cb.

# Interactive Runner black-box smoke test
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/test_blackbox_interactive_runner.sh")
    add_test(
        NAME E2EInteractiveRunner
        COMMAND bash
                "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/test_blackbox_interactive_runner.sh"
                "${CMAKE_BINARY_DIR}/agent_runner"
                "${CMAKE_SOURCE_DIR}/examples/agent/outfit-test-agent"
    )
endif()

# DataBus Client SDK black-box integration test (Python SDK + CLI)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/test_blackbox_databus_clients.sh")
    add_test(
        NAME E2EDataBusClients
        COMMAND bash
                "${CMAKE_CURRENT_SOURCE_DIR}/blackbox/test_blackbox_databus_clients.sh"
                "${CMAKE_BINARY_DIR}/agent_runner"
                "${CMAKE_SOURCE_DIR}/examples/agent/multi_language_databus"
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

# LLM Query Agent Scenario (black-box test through message interface)
if(TARGET llm_query_node AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/llm_query_agent/test_llm_query_agent_e2e.cpp")
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

# [REMOVED] SystemAgentMock Scenario — depends on removed SO libraries (skill_selection_node etc.)
#if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/system_agent_mock/test_system_agent_mock_e2e.cpp")
#    add_executable(test_e2e_system_agent_mock
#        system_agent_mock/test_system_agent_mock_e2e.cpp
#    )
#    target_link_libraries(test_e2e_system_agent_mock
#        agentflow
#        GTest::gtest
#        GTest::gtest_main
#    )
#    add_test(NAME E2ESystemAgentMock COMMAND test_e2e_system_agent_mock)
#    add_dependencies(e2e_tests test_e2e_system_agent_mock)
#    add_dependencies(test_e2e_system_agent_mock
#        skill_selection_node
#        travel_node
#        shopping_node
#        foodie_node
#        coordinator_node
#    )
#endif()
#
# [REMOVED] Stale WORKFLOW_COMPLETE regression E2E — depends on removed SO libraries
#if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/system_agent_mock/test_stale_workflow_completion_e2e.cpp")
#    add_executable(test_e2e_stale_workflow_completion
#        system_agent_mock/test_stale_workflow_completion_e2e.cpp
#    )
#    target_link_libraries(test_e2e_stale_workflow_completion
#        agentflow
#        GTest::gtest
#        GTest::gtest_main
#    )
#    add_test(NAME E2EStaleWorkflowCompletion COMMAND test_e2e_stale_workflow_completion)
#    add_dependencies(e2e_tests test_e2e_stale_workflow_completion)
#    add_dependencies(test_e2e_stale_workflow_completion
#        skill_selection_node
#        travel_node
#        shopping_node
#        foodie_node
#        coordinator_node
#    )
#endif()
#
# [REMOVED] Event Handler Blackbox Test — depends on removed SO libraries
#if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/event_handler/test_event_handler_blackbox.cpp")
#    add_executable(test_e2e_event_handler_blackbox
#        event_handler/test_event_handler_blackbox.cpp
#    )
#    target_link_libraries(test_e2e_event_handler_blackbox
#        agentflow
#        GTest::gtest
#        GTest::gtest_main
#    )
#    add_test(NAME E2EEventHandlerBlackbox COMMAND test_e2e_event_handler_blackbox)
#    add_dependencies(e2e_tests test_e2e_event_handler_blackbox)
#    add_dependencies(test_e2e_event_handler_blackbox
#        skill_selection_node
#        travel_node
#        shopping_node
#        foodie_node
#        coordinator_node
#    )
#endif()
#
# DataBus Bus-B cleanup black-box E2E test (Python)
# 验证 store_with_ref 反向映射修复：对话完成时 Bus-B 清理日志应出现
# 需要 agentflowd 已编译 + websockets 库
find_program(PYTHON3_BIN python3)
if(PYTHON3_BIN AND EXISTS "${CMAKE_SOURCE_DIR}/frontend/bridge/tests/test_databus_cleanup_e2e.py")
    add_test(
        NAME E2EDataBusCleanupDaemon
        COMMAND ${PYTHON3_BIN} -m pytest -x -v
                "${CMAKE_SOURCE_DIR}/frontend/bridge/tests/test_databus_cleanup_e2e.py"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )
    set_tests_properties(E2EDataBusCleanupDaemon PROPERTIES
        ENVIRONMENT "PYTHONPATH=${CMAKE_SOURCE_DIR}/frontend/bridge"
    )
endif()
#if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/system_agent_mock/test_multi_session_concurrent.cpp")
#    add_executable(test_e2e_multi_session_concurrent
#        system_agent_mock/test_multi_session_concurrent.cpp
#    )
#    target_link_libraries(test_e2e_multi_session_concurrent
#        agentflow
#        GTest::gtest
#        GTest::gtest_main
#    )
#    add_test(NAME E2EMultiSessionConcurrent COMMAND test_e2e_multi_session_concurrent)
#    add_dependencies(e2e_tests test_e2e_multi_session_concurrent)
#    add_dependencies(test_e2e_multi_session_concurrent
#        skill_selection_node
#        travel_node
#        shopping_node
#        foodie_node
#        coordinator_node
#    )
#endif()
