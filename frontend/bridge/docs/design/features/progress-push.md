# Coordinator Progress Push 设计文档

## 概述

当 coordinator 决策并派发 skill 后，用户在等待 skill 执行完成期间无法获得任何中间反馈。
本方案在 coordinator 启动 skill 后立即推送一条非终态（`final=false`）的进度通知，
bridge 收到后标记为 `progress=true` 的 chunk 显示在前端，让用户实时感知进度。

## 问题分析

### 原始链路

```
coordinator_decide.py (子进程)
  → stdout 被 CodeLoader 捕获
  → databus 写入 response key + next_skills key
  → C++ COORDINATORDataExchange handler 启动 skill
  → 等待 skill 完成（无推送）
  → skill 完成 → handler 读到 stale response key → 以 final=true 推送
  → 对话结束，用户拿到的是 dispatch 阶段的 progress 消息，不是最终结果
```

### 瓶颈

C++ 端的 `COORDINATORDataExchange` handler（`src/agent/agent.cpp`）只在对话完成时
才调用 `session_router_->enqueue_response_message()` 推送给 bridge。
在 skill 执行期间（可能长达数分钟），没有任何消息推送到前端。

## 解决方案

在 `COORDINATORDataExchange` handler 中，找到 `next_skills` 且所有 skill 都已启动后，
立即读取 databus 中的 `response` key，以 `final=false` 推送给 bridge。

### 插入位置

`src/agent/agent.cpp` L1887，`process_next_skills_launch` 返回之后：

```cpp
if(result.found_next_skills) {
    // ── 新增：all_launched + response key 存在 → 推 non-final progress ──
    if(result.all_launched) {
        DataVariant progress_value;
        if(BuiltinDispatcher::get_framework_data(..., "response", progress_value)) {
            if(const auto* text = std::get_if<std::string>(&progress_value)) {
                MMAgentMsgQue::Message progress_msg = message;
                progress_msg.content = *text;
                progress_msg.metadata["final"] = "false";
                session_router_->enqueue_response_message(progress_msg);
            }
        }
    }
    // ── 原有的 response key 检查逻辑不变 ──
    if(result.all_launched) {
        // 检查 pending_skills_count ...
        // 检查 response key ...
    }
}
```

### 安全保证

| 风险 | 措施 |
|------|------|
| 过早触发 `cleanup_dialogue_data` | 新增代码不调用任何 cleanup 函数 |
| 过早触发 `DIALOGUE_DESTROYED` | 新增代码不调用 `dialogue_event_callback_` |
| bridge 拿到非终态后关闭连接 | `final=false` → bridge 保留 response queue |
| RESPONSE_POLLER 移除 in_flight 请求 | `final=false` → ResponsePoller 不移除 |
| 最终 response 无法到达 | response key 保留在 databus，后续"Response ready"路径正常读取 |

### 端到端消息流

```
coordinator_decide.py (子进程)
  → [PROGRESS]正在执行 skill-1，完成后会自动更新…
  → next_skills = [skill-1]
  → workflow 完成
  → COORDINATORDataExchange handler
  → process_next_skills_launch → all_launched=true
  → 读 response key → {"final":"false", content: "[PROGRESS]..."}
  → session_router_.enqueue_response_message(progress_msg)
  → RESPONSE_POLLER 50ms 轮询 → 取到消息
  → 检测 final=false → 保留 in_flight
  → WebSocket agent.response 通知 → bridge
  → _run_turn 收到 → 检测 [PROGRESS] 前缀
  → ChatChunk{content: "正在执行 skill-1…", final: false, progress: true}
  → 前端显示进度条/灰字提示
  → skill 完成 → coordinator 第2轮
  → final response → {"final":"true", content: "..."}
  → RESPONSE_POLLER 取到 → in_flight 移除
  → bridge 收到 → final=true → 结束对话
```

## 文件变更

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `src/agent/agent.cpp` | 修改 | COORDINATORDataExchange handler 新增 progress push |
| `examples/agent/outfit_assistant/nodes/coordinator_decide.py` | 修改 | P0条件调用collect_skill_results + P1 dispatch后删除response key |
| `examples/agent/outfit_assistant/nodes/databus_client.py` | 修改 | 新增 delete() 方法 |
| `tests/unit/src/test_agent_gtest.cpp` | 新增 | `CoordinatorPushesNonFinalProgressBeforeFinalResponse` 测试 |
| `examples/agent/outfit_assistant/tests/test_coordinator_decide_databus.py` | 新增 | P0/P1 Python 单元测试 |

## 测试覆盖

### C++ 单元测试（compile + run）

```bash
cd build && cmake --build . --target test_agent_gtest -j4
./tests/test_agent_gtest --gtest_filter="*PushesNonFinal*"
```

验证：
1. progress notification 被推送到 response queue
2. metadata `final` = `false`（非终态）
3. content 包含 `[PROGRESS]` 前缀
4. 后续 final response 仍可正常推送

### Python 单元测试

```bash
cd examples/agent/outfit_assistant
python3 -m pytest tests/
```

验证：
1. `test_collect_skill_results_not_called_when_env_var_has_result` — P0
2. `test_collect_skill_results_called_when_env_var_empty` — P0 fallback
3. `test_response_key_deleted_after_dispatch` — P1

## 其他 C++ 推送路径汇总

`session_router_->enqueue_response_message()` 的所有调用点：

| 位置 | 时机 | final 标记 |
|------|------|-----------|
| `agent.cpp` L1737 | Dispatcher 直接响应 | N/A（旧路径） |
| `agent.cpp` L1925 | COORDINATORDataExchange multi-response | `is_done` |
| `agent.cpp` L1959 | COORDINATORDataExchange single response（旧"Response ready"路径） | `"true"` |
| `agent.cpp` L1997 | COORDINATORDataExchange multi-response（无 next_skills） | `is_done` |
| `agent.cpp` L2048 | COORDINATORDataExchange "Coordinator completed without skill" | `"true"` |
| **`agent.cpp` L1896** | **本次新增：progress push** | **`"false"`** |
