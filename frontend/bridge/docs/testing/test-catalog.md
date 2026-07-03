# Bridge 全局测试目录

> 更新日期：2026-07-03
> 设计来源：[e2e-test-design.md](e2e-test-design.md)

状态说明：`待实现` 表示尚未落成自动化；`部分实现并验证` 表示已有稳定的集成/契约保护，但设计要求的完整进程链路仍待补齐；`已实现并验证` 表示对应设计层级已运行通过。

| TC ID | 约束ID | 类型 | 自动化位置 | 状态 | 备注 |
|-------|---------|------|------------|------|------|
| TC-brg-001 | B1,B2 | 正常/E2E | `frontend/bridge/tests/test_bridge_process_e2e.py#test_rest_main_chain_returns_result` | 已实现并验证 | 真实 Bridge + daemon + 脚本化 LLM |
| TC-brg-002 | B1,B2 | 边界/E2E | `frontend/bridge/tests/test_bridge_process_e2e.py#test_ws_large_response_is_complete` | 已实现并验证 | 192KB 完整字节与唯一终态 |
| TC-brg-003 | B2 | 契约/E2E | `frontend/bridge/tests/test_bridge_process_e2e.py#test_rest_and_ws_public_contracts` | 已实现并验证 | 真实进程；不暴露 session_id |
| TC-brg-004 | B1 | 异常/E2E | `frontend/bridge/tests/e2e/test_chat_transport.py#test_unresponsive_daemon_fails_finitely` | 待实现 | timeout 与清理 |
| TC-brg-005 | B3 | 状态/E2E | `frontend/bridge/tests/test_bridge_process_e2e.py#test_same_identity_preserves_context` | 已实现并验证 | UUID 会话与多轮历史连续 |
| TC-brg-006 | B3,B12 | 隔离/E2E | `frontend/bridge/tests/test_bridge_process_e2e.py#test_different_identities_are_isolated` | 已实现并验证 | 防跨用户泄漏 |
| TC-brg-007 | B4 | 回归/E2E | `frontend/bridge/tests/e2e/test_session_isolation.py#test_repeated_skill_dispatch_has_isolated_history` | 待实现 | 重复委派隔离 |
| TC-brg-008 | B5 | 回归/E2E | `frontend/bridge/tests/test_bridge_process_e2e.py#test_truncated_tool_call_recovers_without_execution` | 已实现并验证 | `finish=length` 且残缺工具零副作用 |
| TC-brg-009 | B1,B5 | 异常/E2E | `frontend/bridge/tests/test_bridge_process_e2e.py#test_invalid_tool_json_exhaustion_returns_terminal_result` | 已实现并验证 | 重试耗尽仍返回终态 |
| TC-brg-010 | B6 | 回归/E2E | `frontend/bridge/tests/test_bridge_process_e2e.py#test_artifact_survives_later_model_failure` | 已实现并验证 | 真实 PNG、后续 LLM 失败、聊天 URL 可下载 |
| TC-brg-011 | B7,B9 | 正常/Integration | `frontend/bridge/tests/test_artifacts.py#test_image_is_fetchable_with_real_mime` | 已实现并验证 | 字节、MIME 与完整聊天链路均覆盖 |
| TC-brg-012 | B7,B8 | 组合/Integration | `frontend/bridge/tests/test_artifacts.py#test_html_relative_resource_tree_is_fetchable` | 部分实现并验证 | HTML 相对资源树已覆盖 |
| TC-brg-013 | B10 | 配置/Contract | `frontend/bridge/tests/test_chat_artifact_contract.py#test_public_and_relative_artifact_url_modes` | 部分实现并验证 | REST + WS 改写规则已覆盖 |
| TC-brg-014 | B9 | 安全/Integration | `frontend/bridge/tests/test_artifacts.py#test_artifact_path_escape_is_rejected` | 已实现并验证 | 参数化路径、symlink、类型白名单 |
| TC-brg-015 | B11 | 生命周期/E2E | `frontend/bridge/tests/test_daemon_lifecycle_e2e.py#test_graceful_shutdown_and_restart` | 已实现并验证 | 真实 daemon 请求、SIGTERM、同端口重启 |
| TC-brg-016 | B1,B11,B12 | 稳定性/E2E | `frontend/bridge/tests/test_bridge_process_e2e.py#test_repeated_turns_do_not_degrade_daemon` | 已实现并验证 | 连续 6 轮并检查健康状态 |
| TC-brg-017 | B12 | 并发/E2E | `frontend/bridge/tests/test_bridge_process_e2e.py#test_concurrent_turns_do_not_cross_route` | 部分实现并验证 | 快慢请求不串线；skill 完成顺序待补 |
| TC-brg-018 | B13 | Canary/E2E | `frontend/bridge/tests/e2e/test_live_provider.py#test_live_provider_failure_classification` | 待实现 | Nightly 非阻塞 |
| TC-brg-019 | B14 | 契约/E2E | `frontend/bridge/tests/test_coremind_sidecar_e2e.py#test_sidecar_local_platform_contract` | 已实现并验证 | 真实 C++ SideCar 二进制与健康/管理端点 |
| TC-brg-020 | B14 | 正常/E2E | `frontend/bridge/tests/test_coremind_sidecar_e2e.py#test_sidecar_builds_trusted_context_for_bridge` | 已实现并验证 | 三个入口统一生成内部 envelope 并单向转发 Bridge |
| TC-brg-021 | B14 | 异常/E2E | `frontend/bridge/tests/test_coremind_sidecar_e2e.py#test_sidecar_reports_dependency_failures` | 已实现并验证 | Bridge 502 与 CoreMind-disabled 503/partial |
| TC-brg-022 | B14 | 安全/Contract | `frontend/bridge/tests/test_coremind_internal.py#test_internal_callback_rejects_untrusted_context` | 已实现并验证 | 未认证上下文不进入 AgentFlow |
| TC-brg-023 | B14 | 契约/Integration | `frontend/bridge/tests/test_coremind_internal.py#test_internal_callback_forwards_context_to_agentflow` | 已实现并验证 | request_id 与 platform_context 原样提交 daemon |
| TC-brg-024 | B14 | 正常/E2E | `frontend/bridge/tests/test_coremind_sidecar_e2e.py#test_user_request_reaches_node_with_trusted_context` | 已实现并验证 | 真实 SideCar → Bridge → agentflowd → Node |
| TC-brg-025 | B14 | 异常/Contract | `frontend/bridge/tests/test_coremind_internal.py#test_internal_callback_rejects_inconsistent_service_snapshot` | 已实现并验证 | 不一致服务状态在 Bridge 边界拒绝 |
| TC-brg-026 | B15 | 回归/E2E | `frontend/bridge/tests/test_bridge_process_e2e.py#test_session_messages_preserves_assistant_replies` | 已实现并验证 | coordinator final_response 路径必须把 assistant 回复持久化到 session_messages，跨轮 LLM 调用能找回完整 user+assistant 交替记录 |

## 约束覆盖

| 约束 | 对应 TC |
|------|---------|
| B1 | TC-brg-001、002、004、009、016 |
| B2 | TC-brg-001、002、003 |
| B3 | TC-brg-005、006 |
| B4 | TC-brg-007 |
| B5 | TC-brg-008、009 |
| B6 | TC-brg-010 |
| B7 | TC-brg-011、012 |
| B8 | TC-brg-012 |
| B9 | TC-brg-011、014 |
| B10 | TC-brg-013 |
| B11 | TC-brg-015、016 |
| B12 | TC-brg-006、016、017 |
| B13 | TC-brg-018 |
| B14 | TC-brg-019、020、021、022、023、024、025 |
| B15 | TC-brg-026 |
