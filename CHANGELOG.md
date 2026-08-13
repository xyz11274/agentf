# Changelog

本项目变更记录。格式：`<日期> <类型>: <摘要>`，类型遵循 Conventional Commits。

## 2026-08-10

### fix: 性能基线工具链 3 处缺陷修复（本轮）

修复基线跑测数据无效的根因（首次基线响应 0/20/32 字符、skill 链路未走完是工具链 bug 假象，agent 业务链路本身正常）：

- `src/daemon/rpc_handler.cpp` — `handle_agent_poll_response` complete 判定修正：返回式不再要求 `msg_count > 0`，改为 `has_final || (running_nodes == 0 && queue_empty)`，与循环内 break 条件一致。原实现 final 消息被 ResponsePoller 异步消费后永远返回 false，导致 poll 端等不到完成信号。
- `frontend/bridge/tests/e2e_helpers.py` — `poll_agent_response` 修正：删除对 `workflow.list_instances` 的 `status` 字段依赖（daemon 的 `handle_workflow_list_instances` 根本不返回 status，导致旧 fallback 永远误判"无运行中 workflow"→ 提前返回空）；改为只认 final 消息，complete 时继续轮询等待。
- `frontend/bridge/tests/e2e_perf_baseline.py` — `_parse_daemon_log` LLM 调用统计正则修正：`latency=(\d+)ms` 误匹配 `route latency`/`create_latency` 等字段，改为 `\[llm_call\].*?<<<.*?latency=(\d+)ms` 精确匹配（llm_calls 从 20 修正为 14）。

### perf: outfit agent 性能基线建立（首份有效基线）

- 首次有效基线：`logs/tests/20260810_123056_e2e_perf_baseline/baseline_report.json`
- 3 轮真实 LLM + e2b 完整链路：70.8s/61.5s/11.7s，响应 344/424/367 字符
- 分阶段：sandbox 481ms、coordinator 4544ms、skill_executor 46846ms、LLM 14 次共 118.9s（平均 8.5s/次）、工具 bash×11+read_file×1
- 优化分析记录：`.agents/.coding_gumdam/workspace/.agent-improver/reports/20260810-outfit-perf-baseline-optimization.md`
