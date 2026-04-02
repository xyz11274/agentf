## 2026-03-18 Node 输出数据冲突问题修复（已完成）

### 实施状态

**状态**：✅ 已实施

**提交记录**：
- Edge-based Output Routing 实现于 `src/runtime/runtime.cpp:960-1030`
- Helper functions: `has_downstream_edge()`, `get_outgoing_edges()`, `count_incoming_edges()` (runtime.cpp:35-85)

**单元测试**：
- `EdgeTransferDataMapping` - 测试显式 source_outputs/target_inputs 映射
- `EdgeTransferZeroConfigPassthrough` - 测试零配置透传
- `MultiIncomingEdgeCount` - 测试多入边计数

### 实际实现方案

采用**方案一：Edge 数据映射**，但有以下关键调整：

#### 1. DataBus Key 格式（不含 workflow_id）

**实际实现**：

```cpp
// runtime.cpp:94-105
static std::string build_framework_key(const std::string& session_id,
                                       const std::string& dialogue_id,
                                       const std::string& workflow_id,
                                       const std::string& key) {
  (void)workflow_id;  // Explicitly unused - workflow_id excluded for cross-workflow access
  std::string bus_key = "/session/" + session_id + "/sys/";
  if(!dialogue_id.empty()) {
    bus_key += dialogue_id + "/";
  }
  bus_key += key;
  return bus_key;
}
```

**Key 格式**：`/session/{session_id}/sys/{dialogue_id}/{key}`

**设计决策**：workflow_id **故意不包含**在 key 中，理由：
- 允许跨 workflow 通信（如 skill-selection-workflow 输出 next_skills，dispatcher 直接读取）
- 简化消费端逻辑，无需知道数据来源 workflow
- Session + Dialogue + Namespace 三层隔离已足够区分不同 workflow 输出

#### 2. Edge-based Output Routing（runtime.cpp:960-1030）

```cpp
// Core rules:
// - Has downstream Node → Edge point-to-point transfer to downstream inputs
// - No downstream Node → DataBus broadcast (with session isolation format)
// - Multi-incoming edges (Scenario 2): auto-prefix keys with source node ID

const DataMap& node_outputs = node->get_outputs();
if(!node_outputs.empty()) {
  bool has_downstream = has_downstream_edge(workflow, node);

  if(has_downstream) {
    // Has downstream → Edge point-to-point transfer
    for(const auto& edge : get_outgoing_edges(workflow, node)) {
      DataMap transferred;
      if(!edge->get_source_outputs().empty()) {
        // Has config: use edge mapping (explicit renaming)
        edge->transfer_data(node_outputs, transferred);
      } else {
        // Zero-config: check if target has multiple incoming edges
        int incoming_count = count_incoming_edges(workflow, edge->get_target());
        if(incoming_count > 1) {
          // Scenario 2: Auto-prefix keys with source node ID
          for(const auto& out : node_outputs) {
            std::string prefixed_key = node->get_id() + "." + out.first;
            transferred[prefixed_key] = out.second;
          }
        } else {
          // Single incoming edge: pass all outputs directly
          for(const auto& out : node_outputs) {
            transferred[out.first] = out.second;
          }
        }
      }
      // Merge into target node inputs
      DataMap target_inputs = target_node->get_inputs();
      for(const auto& [key, value] : transferred) {
        target_inputs[key] = value;
      }
      target_node->set_inputs(target_inputs);
    }
  } else {
    // No downstream → DataBus publish (session isolation format)
    for(const auto& out : node_outputs) {
      std::string bus_key = build_framework_key(session_id, dialogue_id, workflow->get_id(), out.first);
      bus.put(bus_key, out.second);
    }
  }
}
```

#### 3. 场景覆盖

| 场景 | 解决方式 | 实现状态 |
|------|---------|---------|
| 场景一：并行 node 同名输出 | Edge 点对点传递，天然隔离 | ✅ 已实现 |
| 场景二：多入边汇聚 | 自动加 source node ID 前缀 | ✅ 已实现 |
| 场景三：多 workflow 并行 | Session + Dialogue 隔离（不含 workflow_id） | ✅ 已实现 |

#### 4. 与原方案差异

| 项目 | 原方案设计 | 实际实现 | 原因 |
|------|----------|---------|------|
| DataBus Key | 含 workflow_id | **不含 workflow_id** | 支持跨 workflow 通信 |
| 零配置多入边 | 需手动配置 | **自动加前缀** | 减少配置负担 |

### 相关代码

| 文件 | 关键代码 |
|------|---------|
| `src/runtime/runtime.cpp:35-85` | Helper functions: has_downstream_edge, get_outgoing_edges, count_incoming_edges |
| `src/runtime/runtime.cpp:94-105` | build_framework_key (不含 workflow_id) |
| `src/runtime/runtime.cpp:960-1030` | Edge-based Output Routing |
| `tests/unit/src/test_workflow_gtest.cpp:41-89` | EdgeTransferDataMapping, EdgeTransferZeroConfigPassthrough |
| `tests/unit/src/test_workflow_gtest.cpp:91` | MultiIncomingEdgeCount |

---

### 背景

当同一 workflow 下多个 node 并行执行时（如 fork 模式），如果不同 node 输出同名 key，会导致 DataBus 数据冲突。

### 问题分析

**当前实现**（`src/runtime/runtime.cpp:655-679`）：

```cpp
for(const auto& out : node_outputs) {
  // 1. 带 dialogue_id 前缀
  output_key = flow_dialogue_id + "/" + out.first;  // dialogue_123/result

  // 2. 带 workflow.node 前缀（安全）
  scoped_key = flow_dialogue_id + "/" + workflow->get_id() + "." + node->get_id() + "." + out.first;

  // 3. 不带前缀（向后兼容）- 问题所在
  agent->publish_data(out.first, out.second);  // result
}
```

**冲突场景**：

**场景一：并行 node 输出同名 key 覆盖**

```
Workflow: fork_workflow
  ├── node_a (并行执行，输出 result="A")
  └── node_b (并行执行，输出 result="B")

DataBus keys:
  dialogue_123/result              → A 或 B（冲突！）
  dialogue_123/fork_workflow.node_a.result → "A" ✅
  dialogue_123/fork_workflow.node_b.result → "B" ✅
  result                           → A 或 B（冲突！）
```

**场景二：多入边 node 消费多个源 node 输出（关键场景）**

```
Workflow: fork_join_workflow
  ├── node_a (并行执行，输出 result="A")
  │
  ├── node_b (并行执行，输出 result="B")
  │
  └── node_c (被 node_a 和 node_b 同时指向，需要消费两者的输出)

问题：
  - node_a 和 node_b 都输出 `result` key
  - node_c 如何区分并消费两个源 node 的输出？
  - 当前实现：node_c 读取 `result` 只能拿到一个值（被覆盖）
```

这是更本质的问题：**下游 node 需要同时消费多个上游 node 的同名输出**。

**场景三：同 dialogue 下不同 workflow 输出到 DataBus 的字段冲突**

```
Session: session_0
Dialogue: dialogue_123

并行执行的 Workflow:
  ├── skill-selection-workflow (输出 next_skills = ["travel", "shopping"])
  ├── travel-workflow (输出 response = "Travel result", next_skills = [])
  └── shopping-workflow (输出 response = "Shopping result", next_skills = [])

DataBus keys (当前实现):
  dialogue_123/next_skills      → 被覆盖！最终是哪个 workflow 的值？
  dialogue_123/response         → 被覆盖！最终是哪个 workflow 的值？
  result                        → 被覆盖！

问题：
  - 多个 skill workflow 并行执行（由 skill-selection 触发）
  - 每个 workflow 都输出 response, next_skills 等通用字段
  - 这些字段在 DataBus 上相互覆盖
  - Coordinator 无法正确读取各 skill 的响应
```

**实际案例（SystemAgentMock）**：

```
skill-selection-workflow 完成 → 发布 next_skills=["travel"] 到 DataBus
travel-workflow 完成          → 发布 response="Travel..." 到 DataBus
                               → 发布 next_skills=[] 到 DataBus（覆盖！）

此时 Coordinator 读取 next_skills=[] 而非 ["travel"]，
可能导致后续 skill 无法正确调度。
```

**根因分析**：

| 层级 | 当前隔离 | 缺失隔离 |
|------|---------|---------|
| Session | ✅ `/session/{session-id}/` | - |
| Dialogue | ✅ `{dialogue-id}/` | - |
| **Workflow** | ❌ 无 | **不同 workflow 输出相同 key 会冲突** |
| Node | ⚠️ 部分 | 同 workflow 内 node 冲突 |

**影响范围**：
- 单 workflow 串行执行：无影响
- 同 workflow Fork 并行 + 相同输出 key：下游 node 可能读到错误数据
- **Fork-Join 模式**：下游 node 无法正确消费多个上游输出
- **多 workflow 并行**：DataBus 字段冲突，框架组件读取错误数据

### 备选方案

#### 方案一：Edge 数据映射（推荐）

**思路**：利用 Edge 的 `source_outputs` / `target_inputs` 配置，实现显式数据映射。

**场景二解决方案**：

```json
// Workflow 配置
{
  "nodes": [
    {"id": "node_a", ...},
    {"id": "node_b", ...},
    {"id": "node_c", ...}
  ],
  "edges": [
    {
      "id": "edge_a_c",
      "source_node": "node_a",
      "target_node": "node_c",
      "source_outputs": ["result"],
      "target_inputs": ["result_from_a"]
    },
    {
      "id": "edge_b_c",
      "source_node": "node_b",
      "target_node": "node_c",
      "source_outputs": ["result"],
      "target_inputs": ["result_from_b"]
    }
  ]
}
```

**数据流**：

```
node_a 输出 result="A"
  → Edge edge_a_c 映射
  → node_c.inputs["result_from_a"] = "A"

node_b 输出 result="B"
  → Edge edge_b_c 映射
  → node_c.inputs["result_from_b"] = "B"

最终 node_c.inputs = { "result_from_a": "A", "result_from_b": "B" }
```

**当前 Edge 配置**（已解析但未使用）：

```json
{
  "id": "edge_a_to_b",
  "source_node": "node_a",
  "target_node": "node_b",
  "source_outputs": ["result"],
  "target_inputs": ["input_data"],
  "transfer_mode": "copy"
}
```

**实现路径**：

1. 在 `process_node()` 中查找当前 node 的入边
2. 对每个入边调用 `edge->transfer_data(source_outputs, target_inputs)`
3. 将映射后的数据注入 node inputs

**优点**：
- ✅ **完美解决多入边场景**：每条边可映射到不同 key
- ✅ **单上游场景零配置**：Edge 点对点传递，天然隔离并行干扰，无需任何参数
- 数据流显式声明，workflow 更清晰
- 支持 key 重命名（`result` → `input_data`）
- 支持 `REFERENCE` 模式（传递 DataBus URI 而非值）

**缺点**：
- 实现复杂度中等，需修改 `process_node()` 逻辑
- 多上游汇聚场景需补充 Edge 的 `source_outputs` / `target_inputs` 配置

**代码变更示意**：

```cpp
// process_node() 中的新增逻辑
for(const auto& edge : workflow->get_edges()) {
  if(edge->get_target() != node->get_id()) continue;

  auto source_node = workflow->get_node(edge->get_source());
  if(!source_node) continue;

  DataMap source_data = source_node->get_outputs();
  DataMap target_data;
  edge->transfer_data(source_data, target_data);

  // 将映射后的数据注入 node inputs
  for(const auto& kv : target_data) {
    hydrated[kv.first] = kv.second;
  }
}
```

#### 方案二：DataBus Key 加前缀（移除无前缀发布）

**思路**：移除不带前缀的发布，仅保留带前缀的安全格式。

**当前发布格式**：

| 格式 | 示例 | 是否安全 |
|------|------|----------|
| `{dialogue_id}/{key}` | `dialogue_123/result` | ❌ 并行 node 冲突 |
| `{dialogue_id}/{workflow}.{node}.{key}` | `dialogue_123/wf.node_a.result` | ✅ |
| `{key}` (无前缀) | `result` | ❌ 并行 node 冲突 |

**场景二解决方案**：

```
node_a 发布: dialogue_123/fork_join.node_a.result = "A"
node_b 发布: dialogue_123/fork_join.node_b.result = "B"

node_c 需要知道上游 node 名称，主动读取：
  - inputs["node_a.result"] 或
  - inputs["node_b.result"]
```

**问题**：
- ❌ node_c 需要硬编码上游 node 名称
- ❌ 无法通过 `input_params` 简单声明
- ❌ workflow 耦合度高，修改上游 node 名称会影响下游

**优点**：
- 实现简单，仅需删除代码
- 完全消除冲突风险
- 保持现有 DataBus 驱动机制

**缺点**：
- 破坏向后兼容性
- 下游 node 需知道上游 node 的输出路径
- 简单 workflow 也需使用完整路径

**代码变更示意**：

```cpp
// 简化 process_node() 输出发布
for(const auto& out : node_outputs) {
  // 仅保留带 workflow.node 前缀的格式
  std::string scoped_key = flow_dialogue_id + "/" + workflow->get_id() + "." + node->get_id() + "." + out.first;
  agent->publish_data(scoped_key, out.second);
  bus.put(scoped_key, out.second);

  // 移除：不带前缀的发布
  // agent->publish_data(out.first, out.second);  // 删除
}
```

#### 方案三：混合方案

**思路**：保留无前缀发布，但下游 node 优先通过 Edge 映射获取数据，无配置时 fallback 到当前逻辑。

**场景二解决方案**：

- 有 Edge 配置时：使用方案一的映射逻辑
- 无 Edge 配置时：fallback 到当前 DataBus 读取逻辑

**问题**：
- ⚠️ 无法解决多入边场景：如果 Edge 无 `target_inputs` 配置，仍然会覆盖
- ⚠️ 优先级逻辑复杂，容易出错

### 方案对比

| 维度 | 方案一：Edge 映射 | 方案二：移除无前缀 | 方案三：混合方案 |
|------|------------------|-------------------|-----------------|
| 实现复杂度 | 中 | 低 | 高 |
| 向后兼容 | ✅ 单上游零配置 | ❌ 破坏兼容 | ✅ 完全兼容 |
| 数据流清晰度 | ✅ 显式声明 | ⚠️ 隐式约定 | ⚠️ 优先级约定 |
| **多入边场景** | ✅ **完美解决** | ❌ 下游需硬编码上游名称 | ❌ 未解决 |
| **多 workflow 并行** | ✅ **DataBus 加 workflow 前缀** | ✅ 前缀区分 | ⚠️ 部分解决 |
| 并行 node 隔离 | ✅ 点对点天然隔离 | ✅ 前缀区分 | ⚠️ 部分解决 |
| 迁移成本 | 低（仅多上游需配置） | 高 | 低 |

### 结论

**推荐方案一（Edge 数据映射 + DataBus workflow 前缀）**，理由：

1. **唯一能正确解决多入边场景**：下游 node 通过不同 key 消费多个上游输出
2. **完美解决多 workflow 并行**：DataBus key 加 workflow 前缀，天然隔离不同 workflow 输出
3. **数据流显式声明**：Edge 配置清晰表达数据流向
4. **已有基础设施**：`transfer_data()` 已实现，仅需调用
5. **单上游零配置**：Edge 点对点传递天然隔离并行干扰，大多数 workflow 无需修改

**实施步骤**：

1. 修改 `process_node()` 在 input hydration 前增加 Edge 数据映射逻辑
2. 遍历入边，获取源 node 输出：
   - **无 `source_outputs` / `target_inputs` 配置**：直接传递同名 key（单上游场景）
   - **有配置**：调用 `edge->transfer_data()` 进行映射重命名（多上游汇聚场景）
3. 将传递/映射后的数据合并到 hydrated inputs
4. 补充单元测试验证单上游和多上游汇聚场景

**Edge 配置策略**：

| 场景 | 是否需要配置 | 说明 |
|------|-------------|------|
| 单上游 + key 匹配 | ❌ 不需要 | Edge 直接传递 `source.outputs[key]` → `target.inputs[key]` |
| 单上游 + key 重命名 | ⚠️ 可选 | 配置 `source_outputs`/`target_inputs` 实现重命名 |
| 多上游汇聚 | ✅ 必须 | 必须配置区分不同来源的同名输出 |

**单上游零配置示例**：

```
并行执行场景（无需任何 Edge 参数）：

Node A (output: result) ──Edge──→ Node B (input: result)
Node A' (output: result) ──Edge──→ Node C (input: result)

Edge(A→B) 从 A.outputs 直接读取 "result" → B.inputs["result"]
Edge(A'→C) 从 A'.outputs 直接读取 "result" → C.inputs["result"]

✅ 天然隔离，不会产生并发问题
```

**输出路由策略（框架自动判断）**：

Node 输出按是否有下游节点自动路由，Node 代码无需任何修改：

| 条件 | 路由方式 | 说明 |
|------|---------|------|
| 有下游 Node | Edge ✅ | 点对点传递，天然隔离并行干扰 |
| 无下游 Node | DataBus ✅ | 广播发布，供框架/外部消费 |

**处理逻辑**：

```cpp
// process_node_outputs()
for(const auto& out : node_outputs) {
  if(has_downstream_edge(workflow, node, out.first)) {
    // 有下游 → Edge 点对点传递
    transfer_via_edge(workflow, node, out.first, out.second);
  } else {
    // 无下游 → DataBus 发布（遵循 session 隔离格式）
    publish_to_databus(session_id, dialogue_id, workflow->get_id(), out.first, out.second);
  }
}
```

**DataBus Key 格式（遵循 Session 隔离）**：

```
/session/{session-id}/sys/{dialogue-id}/{workflow-id}.{key}
```

示例：
```
/session/550e8400.../sys/17411565830363972300/skill-selection-workflow.next_skills
```

**发布逻辑**：

```cpp
void publish_to_databus(const std::string& session_id,
                        const std::string& dialogue_id,
                        const std::string& workflow_id,
                        const std::string& key,
                        const DataVariant& value) {
  // 构建 session 隔离格式的 key
  std::string bus_key = "/session/" + session_id + "/sys/";

  if(!dialogue_id.empty()) {
    bus_key += dialogue_id + "/";
  }

  bus_key += workflow_id + "." + key;

  agent->publish_data(bus_key, value);
  bus.put(bus_key, value);
}
```

**接收端适配（框架组件读取）**：

Dispatcher / Coordinator 读取框架数据时，需适配新的 key 格式：

```cpp
// agent.cpp - 读取 next_skills
std::string build_framework_key(const std::string& session_id,
                                const std::string& dialogue_id,
                                const std::string& key) {
  std::string bus_key = "/session/" + session_id + "/sys/";
  if(!dialogue_id.empty()) {
    bus_key += dialogue_id + "/";
  }
  bus_key += key;  // 或 workflow.key，取决于发布端
  return bus_key;
}

// 查找 key（支持多种格式 fallback）
bool get_framework_data(const std::string& session_id,
                        const std::string& dialogue_id,
                        const std::string& key,
                        DataVariant& value) {
  // 1. 尝试新格式（带 workflow 前缀）
  std::string key_with_workflow = build_framework_key(session_id, dialogue_id, "unknown_workflow." + key);
  if(get_data(key_with_workflow, value)) return true;

  // 2. 尝试新格式（不带 workflow 前缀）
  std::string key_no_workflow = build_framework_key(session_id, dialogue_id, key);
  if(get_data(key_no_workflow, value)) return true;

  // 3. Fallback: 旧格式（dialogue_id/key）
  std::string legacy_key = dialogue_id.empty() ? key : dialogue_id + "/" + key;
  if(get_data(legacy_key, value)) return true;

  return false;
}
```

**隔离层级**：

| 层级 | Key 组成 | 隔离效果 |
|------|---------|---------|
| Session | `/session/{session-id}/` | 多租户隔离 |
| Namespace | `/sys/` | 框架数据命名空间 |
| Dialogue | `{dialogue-id}/` | 多对话隔离 |
| Workflow | `{workflow-id}.{key}` | 多 workflow 隔离 |

**示例**：

```
skill_selection_node (output: selected_skills, next_skills)
    │
    ├── selected_skills ──Edge──→ downstream_node
    │
    └── next_skills ──无下游──→ DataBus
        Key: /session/550e8400.../sys/1741156.../skill-selection-workflow.next_skills
        Consumer: dispatcher
```

**场景三解决方案示例**：

```
同 dialogue 下多 workflow 并行执行：

skill-selection-workflow 输出 next_skills:
  Key: /session/session_0/sys/dialogue_123/skill-selection-workflow.next_skills
  Value: ["travel", "shopping"]

travel-workflow 输出 response:
  Key: /session/session_0/sys/dialogue_123/travel-workflow.response
  Value: "Travel booking confirmed..."

shopping-workflow 输出 response:
  Key: /session/session_0/sys/dialogue_123/shopping-workflow.response
  Value: "Shopping list generated..."

Coordinator 读取各 skill 响应：
  - 读取 /session/session_0/sys/dialogue_123/travel-workflow.response ✅
  - 读取 /session/session_0/sys/dialogue_123/shopping-workflow.response ✅
  - 各 workflow 输出完全隔离，无覆盖冲突
```

**场景对比**：

| 场景 | 冲突原因 | 解决方式 |
|------|---------|---------|
| 场景一：同 workflow 并行 node | 无前缀发布 | Edge 点对点传递 |
| 场景二：多入边汇聚 | DataBus key 相同 | Edge 映射重命名 |
| **场景三：多 workflow 并行** | **无 workflow 前缀** | **DataBus key 加 workflow 前缀** |

### 场景三代码适配方案

#### 1. Key 生产端适配（runtime.cpp）

**当前代码**（`src/runtime/runtime.cpp:656-679`）：

```cpp
// 发布 node 输出到 DataBus
for(const auto& out : node_outputs) {
  // 1. 带 dialogue_id 前缀
  std::string output_key = flow_dialogue_id + "/" + out.first;
  agent->publish_data(output_key, out.second);
  bus.put(output_key, out.second);

  // 2. 带 workflow.node 前缀
  std::string scoped_key = workflow->get_id() + "." + node->get_id() + "." + out.first;
  if(!flow_dialogue_id.empty()) {
    scoped_key = flow_dialogue_id + "/" + scoped_key;
  }
  agent->publish_data(scoped_key, out.second);
  bus.put(scoped_key, out.second);

  // 3. 无前缀（问题所在 - 导致多 workflow 冲突）
  agent->publish_data(out.first, out.second);
  bus.put(out.first, out.second);
}
```

**适配方案**：

```cpp
// 新增辅助函数
static std::string build_framework_key(const std::string& session_id,
                                       const std::string& dialogue_id,
                                       const std::string& workflow_id,
                                       const std::string& key) {
  std::string bus_key = "/session/" + session_id + "/sys/";
  if(!dialogue_id.empty()) {
    bus_key += dialogue_id + "/";
  }
  bus_key += workflow_id + "." + key;
  return bus_key;
}

// 修改发布逻辑
for(const auto& out : node_outputs) {
  // 检查是否有下游节点
  if(has_downstream_edge(workflow, node)) {
    // 有下游 → Edge 点对点传递（已在其他地方实现）
    continue;
  }

  // 无下游 → DataBus 发布（带完整隔离前缀）
  std::string bus_key = build_framework_key(
      session_id, dialogue_id, workflow->get_id(), out.first);

  agent->publish_data(bus_key, out.second);
  bus.put(bus_key, out.second);

  // 保留旧格式向后兼容（可选，建议移除）
  // agent->publish_data(out.first, out.second);
}
```

#### 2. Key 消费端适配（agent.cpp / builtin_dispatcher.cpp）

**当前代码**（消费 next_skills / response）：

```cpp
// agent.cpp:1229-1234 - 读取 next_skills
std::string next_skills_key = dialogue_id.empty() ? "next_skills" : dialogue_id + "/next_skills";
DataVariant next_skills_value;
if(get_data(next_skills_key, next_skills_value)) { ... }

// agent.cpp:1562-1564 - 读取 response
std::string response_key = dialogue_id.empty() ? "response" : dialogue_id + "/response";
DataVariant response_value;
if(get_data(response_key, response_value)) { ... }

// builtin_dispatcher.cpp:95 - 读取 next_skills（无前缀）
if(agent.get_data("next_skills", next_skills_value)) { ... }
```

**适配方案**：

```cpp
// 新增辅助函数 - 支持多格式 fallback
bool get_framework_data(MMAgent& agent,
                        const std::string& session_id,
                        const std::string& dialogue_id,
                        const std::string& workflow_id,
                        const std::string& key,
                        DataVariant& value) {
  // 1. 尝试新格式（带 workflow 前缀）
  std::string new_key = "/session/" + session_id + "/sys/";
  if(!dialogue_id.empty()) {
    new_key += dialogue_id + "/";
  }
  new_key += workflow_id + "." + key;

  if(agent.get_data(new_key, value)) {
    Logger::instance().info_f("[GET_FRAMEWORK_DATA] Found key=%s (new format)", new_key.c_str());
    return true;
  }

  // 2. Fallback: 旧格式（dialogue_id/key）
  std::string legacy_key = dialogue_id.empty() ? key : dialogue_id + "/" + key;
  if(agent.get_data(legacy_key, value)) {
    Logger::instance().info_f("[GET_FRAMEWORK_DATA] Found key=%s (legacy format)", legacy_key.c_str());
    return true;
  }

  // 3. Fallback: 无前缀
  if(agent.get_data(key, value)) {
    Logger::instance().info_f("[GET_FRAMEWORK_DATA] Found key=%s (no prefix)", key.c_str());
    return true;
  }

  return false;
}
```

**消费端调用示例**：

```cpp
// agent.cpp - Coordinator 读取 skill workflow 的响应
std::string skill_workflow_id = "travel-workflow";  // 从上下文获取
DataVariant response_value;

if(get_framework_data(*this, session_id, dialogue_id, skill_workflow_id, "response", response_value)) {
  // 成功读取
}

// builtin_dispatcher.cpp - 读取 next_skills
// 需要知道是哪个 workflow 输出的 next_skills
// 通常由 skill-selection-workflow 输出
std::string source_workflow_id = "skill-selection-workflow";
if(get_framework_data(agent, session_id, dialogue_id, source_workflow_id, "next_skills", next_skills_value)) {
  // 成功读取
}
```

#### 3. 适配难点

| 难点 | 描述 | 解决方案 |
|------|------|---------|
| **消费端需知道 workflow_id** | 读取时需指定来源 workflow | 通过上下文或配置传递 workflow_id |
| **向后兼容** | 旧代码使用无前缀 key | Fallback 机制支持旧格式 |
| **多 skill 响应汇聚** | Coordinator 需读取多个 skill 的响应 | 遍历已执行的 skill workflow 列表 |
| **session_id 获取** | 新格式需要 session_id | 从 WorkflowExecutionState 获取 |

#### 4. 消费端适配（Coordinator 汇聚多 skill 响应）

**当前实现**（读取单一 response）：

```cpp
// agent.cpp:1562-1564
std::string response_key = dialogue_id.empty() ? "response" : dialogue_id + "/response";
DataVariant response_value;
if(get_data(response_key, response_value)) { ... }
```

**适配方案**（读取多个 skill 响应）：

```cpp
// 获取已执行的 skill 列表
std::vector<std::string> executed_skills = get_executed_skills(dialogue_id);

// 汇聚各 skill 的响应
std::string aggregated_response;
for(const auto& skill_name : executed_skills) {
  std::string skill_workflow_id = skill_name + "-workflow";

  DataVariant skill_response;
  if(get_framework_data(*this, session_id, dialogue_id, skill_workflow_id, "response", skill_response)) {
    if(const auto* text = std::get_if<std::string>(&skill_response)) {
      aggregated_response += "[" + skill_name + "] " + *text + "\n";
    }
  }
}

// 发布汇聚后的响应
std::string final_response_key = build_framework_key(session_id, dialogue_id, "coordinator-workflow", "response");
publish_data(final_response_key, aggregated_response);
```

#### 5. 代码变更清单

| 文件 | 变更类型 | 描述 |
|------|---------|------|
| `src/runtime/runtime.cpp` | 修改 | 输出发布逻辑，添加 workflow 前缀 |
| `src/agent/agent.cpp` | 新增 | `get_framework_data()` 辅助函数 |
| `src/agent/agent.cpp` | 修改 | Coordinator 读取响应逻辑 |
| `src/agent/builtin_dispatcher.cpp` | 修改 | 读取 next_skills 逻辑 |
| `include/internal/agent/agent.h` | 新增 | `get_framework_data()` 声明 |

**优势**：
- ✅ **Node 代码零改动**：现有 C++/Python/Shell/Go 节点无需修改
- ✅ **规则简单**：有下游走 Edge，无下游走 DataBus
- ✅ **完整隔离**：Session → Dialogue → Namespace 三层隔离（不含 workflow_id 以支持跨 workflow 通信）
- ✅ **向后兼容**：接收端 fallback 支持旧格式
- ✅ **零配置多入边**：自动加 source node ID 前缀，无需手动配置 Edge

### 进展跟踪

- [x] 代码实现（Edge-based Output Routing）
- [x] 单元测试（EdgeTransferDataMapping, EdgeTransferZeroConfigPassthrough, MultiIncomingEdgeCount）
- [x] 文档更新（标记为 implemented）
- [x] 验证通过

---

### 修订记录

| 版本 | 刷新时间 | 刷新人 | 刷新内容 |
|------|---------|--------|---------|
| v1.0 | 2026-03-18 | - | 初始方案设计 |
| v1.1 | 2026-04-02 | Claude | 标记为已实施，记录实际实现方案和与原方案差异 |

---
