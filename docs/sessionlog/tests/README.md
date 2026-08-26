# sessionlog 单元测试

> 针对 `docs/sessionlog/` 的提交内容（hook 采集 + reader 校验/重放/编排图）的单元测试。
> 测试**不依赖引擎运行环境**（不连 daemon、不连 DataBus、不需要 agent 包），纯内存/临时目录验证。

## 一、测试内容

### 1. `test_reader.py`（11 项）— reader 纯函数

| 测试 | 验证点 |
|---|---|
| `test_validate_serial_chain_passes` | 完整串行链（A→B→C）校验通过（seq/配对/血缘） |
| `test_validate_seq_not_monotonic` | 同一 trace_id 内 seq 乱序 → 报错 |
| `test_validate_seq_duplicate` | 同一 trace_id 内 seq 重复 → 报错 |
| `test_validate_llm_request_without_response` | llm.request 无配对 llm.response → 报错 |
| `test_build_graph_nodes_are_trace_ids` | 编排图节点 = trace_id（3 个执行单元） |
| `test_build_graph_dispatch_edge_exec_key` | 派发边 exec_key 精确匹配 |
| `test_build_graph_return_edge_serial` | 回传边 B→C（新决策段，非 B→A） |
| `test_build_graph_parallel_skills_distinct` | 一次派发 2 个并行 skill → 2 条派发边（exec_key 区分） |
| `test_build_graph_dispatch_fallback_name` | 无 exec_key 时名称+时序兜底 |
| `test_load_torn_line_dropped` | torn 尾行（坏 JSON）丢弃 |
| `test_replay_restores_messages` | 重放恢复 request.messages 序列 |

### 2. `test_hook.py`（7 项）— hook 采集（mock 引擎环境）

| 测试 | 验证点 |
|---|---|
| `test_disabled_no_write` | AF_SESSIONLOG_ENABLED=0 → 不落盘 |
| `test_session_start_writes_event` | AGENT_DIALOGUE_START → session.start + header（含 completed_skill） |
| `test_llm_request_ctx_messages` | BEFORE_LLM_CALL → llm.request 带 ctx.messages 全量（实时） |
| `test_llm_response_reads_databus` | AFTER_LLM_CALL → llm.response 从 DataBus 读 response/meta（mock） |
| `test_skill_end_has_exec_key` | skill.end 带 exec_skill_key（AF_EXEC_SKILL_KEY） |
| `test_tool_call_result` | BEFORE/AFTER_TOOL_CALL → tool.call/tool.result（含 duration_ms、嵌套在 LLM 下） |
| `test_event_envelope_schema` | 信封字段全集（type/seq/ts/pid/trace_id/dialogue_id/session_id/node_id/data） |

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
├── test_reader.py     # reader 测试（11 项）
├── test_hook.py       # hook 测试（7 项）
└── README.md          # 本文档
```

## 六、已知边界

- hook 测试 mock 了 DataBus 和 SpanContext，验证的是"事件结构正确落盘"；**真实 DataBus 交互**（index 尾部 call_id 定位、next_skills 读取）未覆盖——那部分依赖真实 UDS，需集成测试（跑真实任务后检查 sessionlog 文件）
- reader 的 `build_graph` 输出是打印文本（非结构化返回），测试用 `redirect_stdout` 捕获断言——若未来改成返回结构化对象，测试需同步调整
