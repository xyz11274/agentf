# sessionlog 单元测试

> 针对 `docs/sessionlog/` 的提交内容（hook 采集 + reader 校验/重放/编排图）的单元测试。
> 测试**不依赖引擎运行环境**（不连 daemon、不连 DataBus、不需要 agent 包），纯内存/临时目录验证。

## 一、测试内容

### 1. `test_reader.py`（24 项）— reader 纯函数

| 测试 | 验证点 |
|---|---|
| `test_validate_serial_chain_passes` | 完整串行链（A→B→C）校验通过（seq/配对/血缘） |
| `test_validate_seq_not_monotonic` | 同一 trace_id 内 seq 乱序 → 报错 |
| `test_validate_seq_duplicate` | 同一 trace_id 内 seq 重复 → 报错 |
| `test_validate_llm_request_without_response` | llm.request 无配对 llm.response → 报错 |
| `test_validate_shared_trace_uses_process_sequence` | 跨进程共享 trace_id 时各进程 seq 独立 |
| `test_validate_missing_source_sequence` | 血缘 seq 必须存在于同一执行进程 |
| `test_validate_dispatch_without_start_or_batch_terminal` | 成功派发后必须有 Skill 启动或框架 Batch 终态 |
| `test_validate_batch_terminal_covers_framework_only_failure` | batch.result 覆盖未进入 Python 的框架失败 |
| `test_validate_skill_start_without_terminal` | skill.start 必须有 skill.end 或 Batch 终态 |
| `test_validate_tool_call_without_result` | tool.call 必须有最终 tool.result |
| `test_validate_dialogue_requires_terminal_event` | dialogue.start 必须有 dialogue.end |
| `test_validate_accepts_legacy_session_lifecycle` | 历史 session.* 生命周期仍可读取和校验 |
| `test_build_graph_nodes_are_trace_ids` | 编排图节点 = trace_id（3 个执行单元） |
| `test_build_graph_shared_trace_keeps_process_nodes_distinct` | 共享 trace 下仍按 pid 区分执行节点 |
| `test_build_graph_dispatch_edge_exec_key` | 派发边 exec_key 精确匹配 |
| `test_build_graph_return_edge_serial` | 回传边 B→C（新决策段，非 B→A） |
| `test_build_graph_parallel_batch_returns_to_one_activation` | 同批多个 skill.end 按 batch_id 回连唯一激活 |
| `test_build_graph_parallel_skills_distinct` | 一次派发 2 个并行 skill → 2 条派发边（exec_key 区分） |
| `test_build_graph_parallel_dispatch_uses_matching_skill_name` | 每条并行派发边使用自身 exec key 对应的 Skill 名 |
| `test_build_graph_framework_only_failure_uses_batch_result` | launch_failed 分支由 batch.result 生成合成节点与回传边 |
| `test_build_graph_dispatch_fallback_name` | 无 exec_key 时名称+时序兜底 |
| `test_load_torn_line_dropped` | torn 尾行（坏 JSON）丢弃 |
| `test_load_finds_hierarchical_dialogue_by_default` | 仅给 dialogue_id 时发现 User/Conversation 下的实体文件 |
| `test_replay_restores_messages` | 重放恢复 request.messages 序列 |

### 2. `test_hook.py`（20 项）— hook 采集（mock 引擎环境）

| 测试 | 验证点 |
|---|---|
| `test_disabled_no_write` | AF_SESSIONLOG_ENABLED=0 → 不落盘 |
| `test_dialogue_start_writes_event` | 唯一 dialogue.start + header，并按 User/Client(Dialogue) 层级写入唯一实体文件 |
| `test_header_agent_falls_back_to_mounted_hook_path` | 未注入包路径时从 Hook 挂载位置恢复 Agent 身份 |
| `test_hierarchy_projection_sanitizes_user_component` | 恶意用户分量不能逃逸 SessionLog 输出根 |
| `test_hierarchy_write_failure_is_fail_open` | 层级文件写入失败时观测逻辑 fail-open |
| `test_skill_completion_writes_activation_not_dialogue_start` | Skill 回流只写 coordinator.activation，保留 batch_id |
| `test_llm_request_ctx_messages` | BEFORE_LLM_CALL → llm.request 带 ctx.messages 全量（实时） |
| `test_llm_response_reads_databus` | AFTER_LLM_CALL → llm.response 从 DataBus 读 response/meta（mock） |
| `test_skill_llm_events_promote_tree_identity` | Skill LLM 三阶段直接携带建树身份字段 |
| `test_skill_end_has_exec_key` | skill.end 带 exec_skill_key、batch_id 和 artifacts |
| `test_batch_result_preserves_framework_terminal_results` | batch.result 保存完整框架分支终态 |
| `test_messages_snapshot_is_append_only` | 多次有效消息保存追加事件而不覆盖 |
| `test_tool_call_result` | Tool Call/最终 Tool Result → 完整参数与结果落盘（含 duration_ms、血缘） |
| `test_artifact_persist_records_content_identity` | Artifact 持久化记录路径、大小、摘要与终态 |
| `test_partial_dispatch_records_rejected_manifest` | 部分分发保存 accepted/rejected 与 batch 身份 |
| `test_failed_dispatch_records_failure_without_stale_exec_key` | 全部分发失败不误用旧 exec key |
| `test_event_envelope_schema` | 信封字段全集（type/seq/ts/pid/trace_id/dialogue_id/node_id/data），且无 Session 身份 |
| `test_torn_sessionlog_line_does_not_consume_next_event` | torn fragment 不吞掉下一条完整事件 |
| `test_dialogue_error_writes_dialogue_terminal` | 未处理异常仍产生可回放 dialogue.end |
| `test_loop_decisions_write_normalized_control_events` | Coordinator/Skill 循环决策归一化为 loop.control |

### 3. `conftest.py` — 公共 fixture

- `hook_mod`：加载 hook 模块（reload 处理模块级 `_ENABLED`），输出目录指向 tmp_path，环境隔离
- `reader_mod`：加载 reader 模块（纯函数无状态）
- `serial_events`：完整串行链（A→B→C）构造数据（ts 递增模拟真实时序）

## 二、运行方法

```bash
# 从 tests 目录运行
cd docs/sessionlog/tests
pytest -v

# 只跑某组
pytest test_reader.py -v
pytest test_hook.py -v

# 带覆盖率（可选，需安装 pytest-cov）
pytest --cov=.. --cov-report=term-missing
```

**环境要求**：Python 3.10+，pytest（Windows 侧已验证 3.11 + pytest 9.1.1 通过）。

**不需要**：daemon、DataBus UDS、agent 包、`agentflow_runtime` 包——测试用 mock 替代引擎环境，纯本地可跑。

## 三、测试隔离机制

| 依赖 | 处理方式 |
|---|---|
| `AF_SESSIONLOG_ENABLED`（模块级） | `importlib` reload + monkeypatch 环境变量 |
| `_output_dir()` | `AF_TRACE_OUTPUT_DIR` 指向 pytest 的 tmp_path |
| `_get_databus()`（UDS 连接） | monkeypatch 成假 DataBus（内存 dict） |
| `_current_span_id()`（SpanContext） | monkeypatch 成固定 span_id |
| 落盘文件 | 每个测试独立 tmp_path，不污染真实 trace_output |

## 四、测试发现并修复的缺陷

**reader `build_graph` 并行 skill 派发边**（0b98bdc 提交）：
- 缺陷：一个 dispatch 派发多个并行 skill 时，只建**一条**派发边（`matched[0]`）
- 修复：exec_key 精确匹配时取**全部**匹配的 start，每个各建一条边
- 触发测试：`test_build_graph_parallel_skills_distinct`

## 五、文件清单

```
docs/sessionlog/tests/
├── conftest.py        # 公共 fixture（hook 加载/串行链数据）
├── test_reader.py     # reader 测试（22 项）
├── test_hook.py       # hook 测试（13 项）
└── README.md          # 本文档
```

## 六、已知边界

- hook 测试 mock 了 DataBus 和 SpanContext，验证的是"事件结构正确落盘"；**真实 DataBus 交互**（index 尾部 call_id 定位、next_skills 读取）未覆盖——那部分依赖真实 UDS，需集成测试（跑真实任务后检查 sessionlog 文件）
- reader 的 `build_graph` 输出是打印文本（非结构化返回），测试用 `redirect_stdout` 捕获断言——若未来改成返回结构化对象，测试需同步调整
