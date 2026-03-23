## 2026-03-17 Skill 参数槽位校验功能

> **状态：已完成** - 2026-03-17

### 背景

当前框架在 `next_skills` 输出后直接启动对应的 skill tasks，缺乏对 skill 执行前提条件的校验能力。某些 skill 可能需要特定的输入参数才能正常执行（如 "travel" skill 需要 destination 和 date 参数）。如果这些参数在运行时不可用，skill 会执行失败或产生无意义结果。

本设计引入参数槽位校验机制，允许在 agent.json 中声明 skill 的预期参数，框架在启动 skill 前校验这些参数是否可用，校验失败时跳过执行并通知 coordinator。

---

### 接口设计

#### 1. agent.json 新增 `expected_parameters` 属性

在 `skill` task 的 `skill` 配置对象中新增可选属性：

```json
{
  "tasks": [
    {
      "id": 3,
      "type": "skill",
      "description": "Travel booking skill",
      "workflow_file": "skill_travel_workflow.json",
      "skill": {
        "name": "travel",
        "config_dir": "runtime",
        "instance_pool_size": 2,
        "expected_parameters": ["destination", "date", "travelers"]
      }
    },
    {
      "id": 4,
      "type": "skill",
      "description": "Search skill - no parameters required",
      "workflow_file": "skill_search_workflow.json",
      "skill": {
        "name": "search",
        "config_dir": "runtime",
        "instance_pool_size": 2
      }
    }
  ]
}
```

**字段定义**：

| 属性 | 类型 | 必填 | 缺省值 | 说明 |
|------|------|------|--------|------|
| `expected_parameters` | `string[]` | 否 | `[]` | Skill 执行所需的参数名列表 |

**解析逻辑**：
- 未配置时解析为空数组 `[]`
- 空数组表示该 skill 无参数要求，框架不做校验

---

#### 2. Workflow 输出新增 `available_parameters`

当 workflow 完成并输出 `next_skills` 时，需同时输出 `available_parameters`。

**传递机制（优先级从高到低）**：

1. **消息 metadata（推荐）**：放入 workflow 完成消息的 metadata 中
2. **DataBus（向后兼容）**：存入 DataBus，用于兼容旧版本

**重要**：`next_skills` 与 `available_parameters` 同步采用此优先级策略，保持一致性。

**优先级设计原因**：
- 同一 dialogue 可能触发多个 workflow（如 dispatcher 和 skill 都输出 next_skills）
- DataBus 存储会导致后执行的 workflow 覆盖前者的输出
- 消息 metadata 与特定 workflow 实例绑定，避免并发覆盖问题

**消息 metadata 结构**：

```cpp
// Workflow 完成消息
MMAgentMsgQue::Message completion_message;
completion_message.content = "workflow_completed";
completion_message.metadata["next_skills"] = R"(["travel", "shopping"])";
completion_message.metadata["available_parameters"] = R"({
  "travel": {
    "destination": "Beijing",
    "date": "2024-03-20"
  },
  "shopping": {
    "category": "electronics"
  }
})";
```

**DataBus 存储（向后兼容）**：

```
/{dialogue_id}/next_skills = ["travel", "shopping"]
/{dialogue_id}/available_parameters = {
  "travel": {
    "destination": "Beijing",
    "date": "2024-03-20"
  }
}
```

**数据结构定义**：

```typescript
// available_parameters 结构（参数值仅支持 string 类型）
{
  [skill_name: string]: {
    [parameter_name: string]: string
  }
}
```

**要点**：
- 参数值仅支持 string 类型
- `available_parameters` 为可选输出，未提供时视为空对象 `{}`
- 框架优先从消息 metadata 读取，其次从 DataBus 读取

---

#### 3. 参数校验逻辑

**校验位置**：`src/agent/agent.cpp` 的 `launch_skill_task()` 函数或其调用处

**校验流程**：

```
┌─────────────────────────────────────────────────────────────────┐
│                     Skill Launch with Validation                │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  1. Get skill template from session                             │
│     └─> skill_template->get_expected_parameters()               │
│                                                                  │
│  2. If expected_parameters is empty                             │
│     └─> Skip validation, launch skill directly                  │
│                                                                  │
│  3. Get available_parameters (priority: metadata > DataBus)     │
│     ├─> First: message.metadata["available_parameters"]         │
│     └─> Fallback: DataBus {dialogue_id}/available_parameters    │
│                                                                  │
│  4. Get next_skills (same priority: metadata > DataBus)         │
│     ├─> First: message.metadata["next_skills"]                  │
│     └─> Fallback: DataBus {dialogue_id}/next_skills             │
│                                                                  │
│  5. For each expected_parameter:                                │
│     └─> Check if exists in available_parameters[skill_name]     │
│                                                                  │
│  6. If all expected parameters are covered:                     │
│     └─> Launch skill task                                       │
│                                                                  │
│  7. If any expected parameter is missing:                       │
│     └─> Skip skill launch                                       │
│     └─> Send failure message to coordinator                     │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

**伪代码**：

```cpp
bool MMAgent::launch_skill_task_with_validation(
    const std::shared_ptr<MMAgentSession>& session,
    const std::string& skill_name,
    const MMAgentMsgQue::Message& context,
    const std::shared_ptr<MMAgentTask>& source_task) {

  // 1. Find skill template
  auto skill_template = find_skill_template(session, skill_name);
  if (!skill_template) return false;

  // 2. Get expected_parameters
  const auto& expected_params = skill_template->get_expected_parameters();

  // 3. If empty, skip validation
  if (expected_params.empty()) {
    return launch_skill_task(session, skill_name, context, source_task);
  }

  // 4. Get available_parameters (priority: metadata > DataBus)
  std::map<std::string, std::string> available_params;

  // 4.1 Try metadata first
  if (context.metadata.count("available_parameters")) {
    available_params = parse_available_parameters(
        context.metadata.at("available_parameters"), skill_name);
  } else {
    // 4.2 Fallback to DataBus
    std::string dialogue_id = context.metadata.count("dialogue_id")
        ? context.metadata.at("dialogue_id") : "";
    std::string params_key = dialogue_id.empty()
        ? "available_parameters"
        : dialogue_id + "/available_parameters";

    DataVariant params_value;
    if (get_data(params_key, params_value)) {
      available_params = parse_available_parameters(params_value, skill_name);
    }
  }

  // 5. Validate: check if all expected parameters are available
  std::vector<std::string> missing_params;
  for (const auto& expected : expected_params) {
    if (available_params.find(expected) == available_params.end()) {
      missing_params.push_back(expected);
    }
  }

  // 6. If validation fails, notify coordinator and return
  if (!missing_params.empty()) {
    send_skill_launch_failure(
        session, skill_name, source_task,
        expected_params, available_params, missing_params);
    return false;
  }

  // 7. Validation passed, inject parameters and launch skill
  inject_skill_parameters(skill_template, available_params);
  return launch_skill_task(session, skill_name, context, source_task);
}
```

---

#### 4. 失败消息格式

当参数校验失败时，框架向 coordinator 发送失败消息：

**消息结构**：

```cpp
MMAgentMsgQue::Message failure_message;
failure_message.message_id = "skill-failure-" + skill_name + "-" + generate_uuid();
failure_message.sender_id = "framework";
failure_message.receiver_id = coordinator_task_id;
failure_message.content = "skill_launch_failed";

// Metadata fields
failure_message.metadata["task_type"] = "skill_response";
failure_message.metadata["message_type"] = "skill_launch_failed";
failure_message.metadata["skill_name"] = skill_name;
failure_message.metadata["failure_reason"] = "parameter_validation_failed";

// Expected vs Available (JSON serialized)
failure_message.metadata["expected_parameters"] = R"(["destination", "date", "travelers"])";
failure_message.metadata["available_parameters"] = R"({"destination": "Beijing", "date": "2024-03-20"})";
failure_message.metadata["missing_parameters"] = R"(["travelers"])";
```

**Metadata 字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `task_type` | string | 固定为 `"skill_response"` |
| `message_type` | string | `"skill_launch_failed"` 标识启动失败 |
| `skill_name` | string | 失败的 skill 名称 |
| `failure_reason` | string | 失败原因，此处为 `"parameter_validation_failed"` |
| `expected_parameters` | string | JSON 序列化的预期参数列表 |
| `available_parameters` | string | JSON 序列化的可用参数对象 |
| `missing_parameters` | string | JSON 序列化的缺失参数列表 |

**Coordinator 处理建议**：
- 可根据 `missing_parameters` 向用户请求补充信息
- 可记录日志用于问题诊断
- 可选择降级到其他 skill 或返回错误响应

---

### 数据流示例

#### 场景：Travel Skill 参数缺失

**1. Dispatcher workflow 执行完成，输出消息：**

```cpp
// Workflow 完成消息
MMAgentMsgQue::Message completion_message;
completion_message.content = "workflow_completed";
completion_message.metadata["next_skills"] = R"(["travel"])";
completion_message.metadata["available_parameters"] = R"({
  "travel": {
    "destination": "Beijing",
    "date": "2024-03-20"
  }
})";
```

**2. agent.json 配置：**

```json
{
  "skill": {
    "name": "travel",
    "expected_parameters": ["destination", "date", "travelers"]
  }
}
```

**3. 框架校验：**
- expected: `["destination", "date", "travelers"]`
- available: `{"destination": "Beijing", "date": "2024-03-20"}`
- missing: `["travelers"]`

**4. 发送给 Coordinator 的失败消息：**

```json
{
  "message_id": "skill-failure-travel-abc123",
  "sender_id": "framework",
  "receiver_id": "coordinator-0",
  "content": "skill_launch_failed",
  "metadata": {
    "task_type": "skill_response",
    "message_type": "skill_launch_failed",
    "skill_name": "travel",
    "failure_reason": "parameter_validation_failed",
    "expected_parameters": "[\"destination\", \"date\", \"travelers\"]",
    "available_parameters": "{\"destination\": \"Beijing\", \"date\": \"2024-03-20\"}",
    "missing_parameters": "[\"travelers\"]"
  }
}
```

---

### 代码变更清单

#### 头文件变更

**`include/internal/agent/package.h`**

```cpp
struct SkillTaskConfig {
  std::string skill_name;
  std::string skill_config_dir;
  int         instance_pool_size = 1;
  std::vector<std::string> expected_parameters;  // 新增
};
```

**`include/internal/agent/task.h`**

```cpp
class MMAgentTask {
public:
  // 新增方法
  const std::vector<std::string>& get_expected_parameters() const;
  void set_expected_parameters(const std::vector<std::string>& params);

private:
  std::vector<std::string> expected_parameters_;  // 新增成员
};
```

#### 源文件变更

**`src/agent/package.cpp`**

- 解析 `expected_parameters` 数组字段

**`src/agent/agent.cpp`**

- 在 `launch_skill_task()` 前增加参数校验逻辑
- 新增 `send_skill_launch_failure()` 方法
- 新增 `parse_available_parameters()` 辅助函数

#### 示例配置变更

需更新的 `agent.json` 示例（按需添加 `expected_parameters`）：

- `examples/agent/system_agent_mock/agent.json`
- 其他包含 skill task 的 agent.json

---

### 文档更新清单

| 文档 | 更新内容 | 状态 |
|------|----------|------|
| `publish/agent/AGENT_PACKAGE_SPEC.md` | 新增 `expected_parameters` 字段说明 | ✅ 已完成 |
| `publish/INTEGRATION_GUIDE.md` | 新增参数校验机制说明、失败消息格式 | ✅ 已完成 |
| `docs/agentflow_user_manual.md` | 新增 workflow 输出 `available_parameters` 说明 | ✅ 已完成 |
| `publish/workflow/NODE_API_REFERENCE.md` | 新增节点如何输出 `available_parameters` | ✅ 已完成 |
| `docs/REFACTORING.md` | 本设计文档 | ✅ 已完成 |

---

### 验证方案

#### 单元测试用例

已实现 8 个单元测试用例：

1. **EmptyExpectedParametersSkipsValidation** - 空 expected_parameters 不触发校验
2. **ParseAvailableParametersFromMetadata** - 从 metadata 解析 available_parameters
3. **ValidationPassWhenAllParametersAvailable** - expected_parameters 全部满足，正常启动 skill
4. **ValidationFailsWhenParametersMissing** - expected_parameters 部分缺失，发送失败消息
5. **NoAvailableParametersTreatsAllAsMissing** - available_parameters 未提供，所有 expected 视为缺失
6. **ExtraAvailableParametersIgnored** - available_parameters 包含额外参数，不影响校验
7. **DataBusFallbackWhenNoMetadataAvailableParameters** - DataBus 回退读取
8. **DialogueIdPrefixedAvailableParametersKey** - dialogue_id 前缀的 DataBus key

#### 验证结果

- `./build.sh build`：✅ 通过
- `ctest --test-dir build -R AgentFlowGoogleTests --output-on-failure`：✅ 通过
- `./build.sh test`：✅ 18/19 测试通过（E2ELLMQueryAgent 因 API Key 缺失失败，与本次修改无关）

#### 提交记录

- `ef98064` - feat: Implement skill parameter slot validation
- `f574039` - test: Add unit tests for skill parameter validation
- `021ac9b` - docs: Add expected_parameters field to agent package spec
- `bb04c8e` - docs: Add skill parameter validation to integration guide
- `c98985f` - docs: Add skill parameter validation to user manual
- `625e062` - docs: Add skill parameter validation to NODE_API_REFERENCE

---

### 待确认事项

**已确认**：
- 参数值仅支持 string 类型
- 不支持参数默认值
- next_skills 与 available_parameters 同步采用 metadata 优先策略
- 失败消息格式符合预期

---

### 向后兼容性

- `expected_parameters` 为可选字段，未配置时行为与当前一致
- `available_parameters` 为可选输出，未提供时视为空对象
- 框架优先从消息 metadata 读取，DataBus 作为 fallback
- 完全向后兼容现有 agent.json 和 workflow

---

### 实施完成

功能已完整实现并验证通过：

1. ✅ 代码变更：`package.h`, `task.h`, `package.cpp`, `agent.cpp`
2. ✅ 单元测试：8 个测试用例全部通过
3. ✅ 文档更新：5 份文档已更新
4. ✅ 集成测试：18/19 测试通过

---
