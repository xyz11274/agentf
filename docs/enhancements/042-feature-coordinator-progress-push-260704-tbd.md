# 详细设计: Coordinator Progress Push

> **文档级别**: L4 技术设计
> **日期**: 2026-07-04
> **状态**: 待审阅
> **关联**: `frontend/bridge/docs/design/features/progress-push.md`（Bridge 侧详细设计）

---

## 1. 背景

### 1.1 问题陈述

当前 coordinator 决策并派发 skill 后，用户在等待 skill 执行完成期间无法获得任何中间反馈。skill 执行可能持续数分钟，前端在无响应期间呈现空白等待状态，用户体验差。

### 1.2 设计目标

- coordinator 启动 skill 后立即推送一条非终态（`final=false`）的进度通知
- bridge 收到后标记为 `progress=true` 的 chunk 显示在前端
- 用户实时感知"正在执行 XX 技能"等进度提示
- 不干扰原有的 final response 推送路径

### 1.3 非目标

- 不做进度百分比或 ETA 预估
- 不做多技能的合并进度（每次只推送当前已启动的技能状态）
- 不做 WebSocket 推送机制的改造（复用现有 `session_router_->enqueue_response_message()` 路径）

---

## 2. 方案简述

在 `COORDINATORDataExchange` handler 中新增一个检测点：当 `process_next_skills_launch` 返回 `all_launched=true` 时，立即读取 databus 中的 `response` key，以 `final=false` 推送给 bridge。

本质是在原有"只在对话完成时推送"的单点基础上，增加一个"skill 全部启动后立即推送进度"的中间点。

---

## 3. 约束与假设

| # | 约束/假设 | 说明 |
|---|-----------|------|
| 1 | bridge 侧已支持 `progress=true` 的 ChatChunk | 该能力由 `frontend/bridge/docs/design/features/progress-push.md` 定义 |
| 2 | databus 中 `response` key 在 dispatch 阶段已写入 | coordinator_decide.py 在 P1 阶段存放 `[PROGRESS]...` 文本 |
| 3 | 新增推送不调用任何 cleanup/destroy 函数 | 防止过早触发 `cleanup_dialogue_data` 或 `DIALOGUE_DESTROYED` |
| 4 | `final=false` 不触发 bridge 端关闭 ResponsePoller 的 in_flight 请求 | RESPONSE_POLLER 根据 `final` 标记决定是否移除 in_flight |
| 5 | `response` key 在读取后保留在 databus | 供后续"Response ready"路径同样的 key 再次读取 |

---

## 4. 当前实现分析

### 4.1 代码分析

**原始链路流程**：

```
coordinator_decide.py (子进程)
  → stdout 被 CodeLoader 捕获
  → databus 写入 response key + next_skills key
  → C++ COORDINATORDataExchange handler 启动 skill
  → 等待 skill 完成（无推送）
  → skill 完成 → handler 读到 stale response key → 以 final=true 推送
  → 对话结束，用户拿到的是 dispatch 阶段的 progress 消息，不是最终结果
```

**核心瓶颈**：

C++ 端的 `COORDINATORDataExchange` handler（`src/agent/agent.cpp`）只在对话完成时才调用 `session_router_->enqueue_response_message()` 推送给 bridge。在 skill 执行期间，没有任何消息推送到前端。

**关键代码位置**：

| 位置 | 行号（参考） | 功能 |
|------|------------|------|
| `COORDINATORDataExchange` handler 入口 | ~L1880 | multipart message 处理函数 |
| `process_next_skills_launch` 调用 | ~L1887 | 解析 next_skills 并启动子 skill |
| `response` key 检查 + enqueue | ~L1925+ | 原有 final response 推送路径 |
| `session_router_->enqueue_response_message()` | 多个位置 | 推送消息到 bridge 的响应队列 |

### 4.2 Examples 分析

**`examples/agent/outfit-assistant/nodes/coordinator_decide.py`**：

该节点负责 coordinator 决策流程，在 dispatch 后写入 `response` key（含 `[PROGRESS]...` 前缀文本）和 `next_skills` key。当前逻辑：

- P0 条件：`RESULT_RESPONSE` 环境变量存在 → 跳过 `collect_skill_results`，仅 dispatch + 写入 response key
- P0 fallback：`RESULT_RESPONSE` 环境变量不存在 → 调用 `collect_skill_results` 采集结果
- P1 独立校验与补充推责

**变更点**：coordinator_decide.py 需要在 dispatch 后**保留** `response` key 给 C++ 端读取（不做删除），并在 P1 中新增对 response key 的清理逻辑（防止 poll 到旧数据）。

**`examples/agent/outfit-assistant/nodes/databus_client.py`**：

当前已有 `get()`、`set()`、`keys()` 方法。新增 progress push 需要 P1 中删除 `response` key 的能力。

**变更点**：新增 `delete(key)` 方法。

### 4.3 缺失功能分析

| # | 缺失功能 | 说明 |
|---|---------|------|
| 1 | C++ 端缺少 skill 启动后的中间推送点 | handler 中需新增一处 `enqueue_response_message()` 调用 |
| 2 | P1 coordinator_decide.py 缺少 dispatch 后清理 response key 的能力 | 需新增 `delete()` 方法，清理旧 response 防止误导 |
| 3 | 缺少 C++ 单元测试覆盖 progress push 场景 | 需要新增 test case 验证 metadata.final=false、content 包含 `[PROGRESS]` 前缀等 |

行业实践中常见做法是 WebSocket 直接推送 `progress` 事件（如 OpenAI 的 SSE stream），但本方案复用已有 `enqueue_response_message` 路径，通过 `final=false` + `[PROGRESS]` 前缀区分进度消息与终态消息，最小化对现有架构的侵入。

---

## 5. 数据结构设计

### 5.1 消息结构

复用 `MMAgentMsgQue::Message`，新增 `metadata["final"]` 字段标记：

| 字段 | 类型 | 说明 |
|------|------|------|
| `content` | string | 进度文本，带 `[PROGRESS]` 前缀 |
| `metadata["final"]` | string | `"false"` 表示非终态进度推送 |

**示例**：

```json
{
  "content": "[PROGRESS]正在执行 skill-1，完成后会自动更新…",
  "metadata": {
    "final": "false"
  }
}
```

### 5.2 现有结构兼容性

`MMAgentMsgQue::Message` 的 `metadata` 字段原用于标记 `"final"` / `"done"`，本次仅在已有字段上使用新值 `"false"`，无需新增字段或修改消息结构。

```cpp
// 原有 final=true 的结构不变
progress_msg.metadata["final"] = "false";  // ✅ 仅新增一个 metadata key-value
```

---

## 6. 流程设计

### 6.1 端到端消息流

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

### 6.2 C++ handler 处理流程（新增点）

```
COORDINATORDataExchange handler 收到 multipart message
  │
  ├─ parse next_skills from databus
  ├─ process_next_skills_launch()
  │
  ├─ all_launched==true ────────────────────────────────────┐
  │                                                         │
  │  ┌─ 新增代码块 ────────────────────────────────────┐   │
  │  │ 读 databus response key                          │   │
  │  │ if (exists AND 有内容) {                          │   │
  │  │   progress_msg.content = *text;                  │   │
  │  │   progress_msg.metadata["final"] = "false";      │   │
  │  │   session_router_->enqueue_response_message();   │   │
  │  │ }                                                │   │
  │  └──────────────────────────────────────────────────┘   │
  │                                                         │
  ├─ 原有代码（不变）                                        │
  │  ├─ all_launched + response key → final=true 推送       │
  │  └─ ...                                                  │
```

### 6.3 安全保证

| 风险 | 措施 |
|------|------|
| 过早触发 `cleanup_dialogue_data` | 新增代码不调用任何 cleanup 函数 |
| 过早触发 `DIALOGUE_DESTROYED` | 新增代码不调用 `dialogue_event_callback_` |
| bridge 拿到非终态后关闭连接 | `final=false` → bridge 保留 response queue |
| RESPONSE_POLLER 移除 in_flight 请求 | `final=false` → ResponsePoller 不移除 |
| 最终 response 无法到达 | response key 保留在 databus，后续"Response ready"路径正常读取 |

---

## 7. 接口设计

### 7.1 内部接口

无新增外部接口。所有修改是 `agent.cpp` 内部的 handler 逻辑增加一处分支，以及 `databus_client.py` 新增一个工具方法。

### 7.2 DataBus 接口

| 接口 | 变更 | 说明 |
|------|------|------|
| `databus.get("response")` | 无变更 | handler 读取现有关键字 |
| `databus.delete("response")` | **新增** | P1 中使用，清理旧 response 数据 |

### 7.3 coordinator_decide.py 内部行为

| 阶段 | 条件 | 行为 |
|------|------|------|
| P0 | `RESULT_RESPONSE` 存在 | dispatch + 写入 response key（含 `[PROGRESS]`），新增 collect_skill_results 跳过逻辑 |
| P0 fallback | `RESULT_RESPONSE` 不存在 | collect_skill_results + dispatch，写入 response key |
| P1 | dispatch 完成后 | 在 wait_response 并校验后调用 `databus.delete("response")`，防止 poll 到旧数据 |

---

## 8. 实现方案

### 8.1 C++ agent.cpp — 新增 progress push 分支

在 `process_next_skills_launch` 返回 `all_launched=true` 后，原有的"检查 response key + push"逻辑之前，插入一段独立的 progress push 逻辑。

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

**实现要点**：
- 完全独立于原有逻辑，不修改任何现有控制流
- 不调用 cleanup、destroy、event_callback 等有副作用的函数
- `final=false` 是关键标记，bridge 和 RESPONSE_POLLER 据此判定为非终态

### 8.2 coordinator_decide.py — P0 增加 collect_skill_results 跳过、P1 增加 delete

**P0 变更**：当 `RESULT_RESPONSE` 环境变量已有结果时，跳过耗时的 `collect_skill_results` 调用，直接进行 dispatch 和 response key 写入。

```python
# P0: 如果已有 RESULT_RESPONSE，跳过 collect_skill_results
if os.environ.get("RESULT_RESPONSE"):
    dispatch_skills(...)
    databus.set("response", "[PROGRESS]正在执行 skill-1，完成后会自动更新…")
else:
    results = collect_skill_results(...)
    dispatch_skills(results, ...)
    databus.set("response", build_progress_text(results))
```

**P1 变更**：在 wait_response 校验通过后，调用 `databus.delete("response")` 清理旧数据。

### 8.3 databus_client.py — 新增 delete() 方法

```python
def delete(self, key: str) -> bool:
    """删除 DataBus 中的指定 key。成功返回 True，key 不存在返回 False。"""
    ...
```

### 8.4 备选方案对比

| 方案 | 优点 | 缺点 |
|------|------|------|
| **方案 A（选定）**：复用 enqueue_response_message + final=false | 最小改动，不破坏现有架构 | 需要 bridge 端配合识别 progress chunk |
| 方案 B：新增独立的 progress 回调接口 | 语义清晰 | 新增接口和消息类型，架构侵入大 |

### 8.5 待修改文件清单

| 文件 | 修改类型 | 说明 |
|------|---------|------|
| `src/agent/agent.cpp` | 修改 | COORDINATORDataExchange handler 新增 progress push（~L1887） |
| `examples/agent/outfit-assistant/nodes/coordinator_decide.py` | 修改 | P0 条件调用 collect_skill_results + P1 dispatch 后删除 response key |
| `examples/agent/outfit-assistant/nodes/databus_client.py` | 修改 | 新增 `delete()` 方法 |
| `tests/unit/src/test_agent_gtest.cpp` | 新增 | `CoordinatorPushesNonFinalProgressBeforeFinalResponse` 测试 |
| `examples/agent/outfit-assistant/tests/test_coordinator_decide_databus.py` | 新增 | P0/P1 Python 单元测试 |

### 8.6 功能点实现优先级排序

| 优先级 | 功能点 | 副作用分析 | 验证策略 |
|--------|--------|-----------|---------|
| P0 | databus_client.py 新增 delete() | 无副作用 | 单元测试 |
| P1 | coordinator_decide.py P0 增加 collect_skill_results 跳过 | 无副作用，RESULT_RESPONSE 存在时行为不变 | 单元测试 + 回归 |
| P1 | coordinator_decide.py P1 dispatch 后删除 response key | 无副作用，旧数据不会被 poll 到 | 单元测试 |
| P2 | C++ agent.cpp 新增 progress push | 无副作用，final=false 不触发 cleanup | C++ 单元测试 compile + run |
| P2 | C++ 单元测试 | 纯新增，不影响现有功能 | compile + run |

---

## 9. 测试方案

### 9.1 Examples 黑盒测试覆盖分析

| 功能点 | 现有 Examples 覆盖 | 更新建议 |
|--------|-------------------|---------|
| coordinator_decide.py P0 跳过 collect_skill_results | ❌ 未覆盖 | 新增 test: `test_collect_skill_results_not_called_when_env_var_has_result` |
| coordinator_decide.py P1 删除 response key | ❌ 未覆盖 | 新增 test: `test_response_key_deleted_after_dispatch` |
| databus_client.py delete() | ❌ 未覆盖 | 新增 test: `test_databus_delete` |

### 9.2 测试用例设计

**C++ 单元测试（compile + run）**：

```bash
cd build && cmake --build . --target test_agent_gtest -j4
./tests/test_agent_gtest --gtest_filter="*PushesNonFinal*"
```

验证：
1. progress notification 被推送到 response queue
2. metadata `final` = `false`（非终态）
3. content 包含 `[PROGRESS]` 前缀
4. 后续 final response 仍可正常推送

**Python 单元测试**：

```bash
cd examples/agent/outfit-assistant
python3 -m pytest tests/
```

验证：
1. `test_collect_skill_results_not_called_when_env_var_has_result` — P0
2. `test_collect_skill_results_called_when_env_var_empty` — P0 fallback
3. `test_response_key_deleted_after_dispatch` — P1

### 9.3 独立性要求

- C++ 和 Python 测试互相独立，无交叉依赖
- 测试数据由测试框架自身构建，不依赖外部系统
- Databus 的 mock/stub 通过测试 fixture 注入，不修改功能代码

---

## 10. 文档

### 10.1 需要变更的文档

| 文档 | 变更说明 |
|------|---------|
| `frontend/bridge/docs/design/features/progress-push.md` | 已编写完成，本 enhancement 是其 C++ 侧的对应方案文档 |
| `docs/enhancements/README.md` | 新增索引行（本文件） |

### 10.2 关联方案

| 关联方案 | 关系说明 |
|---------|---------|
| [frontend/bridge/docs/design/features/progress-push.md](../../frontend/bridge/docs/design/features/progress-push.md) | Bridge 侧详细设计，定义 `progress=true` ChatChunk 及前端渲染；本文档是其 C++ 框架侧的对应实现方案 |

---

## 11. 进展跟踪

- [ ] 代码实现
  - [ ] P0: databus_client.py 新增 delete()
  - [ ] P1: coordinator_decide.py P0/P1 逻辑修改
  - [ ] P2: agent.cpp 新增 progress push
- [ ] 单元测试
  - [ ] Python: test_coordinator_decide_databus.py
  - [ ] C++: test_agent_gtest.cpp 新增 case
- [ ] 集成测试
- [ ] 文档更新
  - [ ] docs/enhancements/README.md 索引更新
- [ ] 验证通过

### 详细进度

| 功能点 | 功能 | 用例 | 文档 | 状态 |
|--------|------|------|------|------|
| databus_client.py delete() | ⏳待实施 | ⏳待实施 | ✅已就绪 | 待开始 |
| coordinator_decide.py P0/P1 | ⏳待实施 | ⏳待实施 | ✅已就绪 | 待开始 |
| agent.cpp progress push | ⏳待实施 | ⏳待实施 | ✅已就绪 | 待开始 |
| C++ 单元测试 | ⏳待实施 | ⏳待实施 | ✅已就绪 | 待开始 |
| Python 单元测试 | ⏳待实施 | ⏳待实施 | ✅已就绪 | 待开始 |

---

## 12. 后续计划（可选）

| 优化项 | 预期效果 | 方案简述 |
|--------|---------|---------|
| 多技能合并进度 | 多个 skill 并行时可合并显示为 "正在执行 N 个技能…" | 在 coordinator_decide.py 汇总 pending skills 并生成合并进度文本 |

---

## 13. 修订记录

| 版本 | 刷新时间 | 刷新人 | 刷新内容 |
|------|---------|--------|---------|
| v1.0 | 2026-07-04 | 初始创建 | 基于 `frontend/bridge/docs/design/features/progress-push.md` 的 C++ 框架侧方案文档 |
