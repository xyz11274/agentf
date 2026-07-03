## 2026-03-19 消息定向路由机制设计

### 背景

当前消息路由流程需要经过多层遍历：

```
外部消息 → SessionRouter → Agent::handle_route_message → 
session->enqueue_received_message → 默认放入 dispatcher 队列 →
Agent::process_intent_request_loop 遍历所有 task → 分发到目标 task 队列
```

**问题**：
- 消息需要经过多层遍历才能到达目标 task
- 外部服务无法直接向特定 task（如正在运行的 skill）发送消息

**需求**：
如果外部消息携带了 `dialogue_id` 和 `task_id`，则消息直接发送到对应 task 的队列，跳过中间遍历。

### 当前消息路由流程

```
外部消息
    │
    ▼
SessionRouter::route_request_message()
    │
    ▼ (放入 request_queue_)
    │
    ▼
handle_route_message()
    │ 根据 user_id:client_id 或 session_id 查找 session
    │
    ▼
route_handler_(session, message)  // 即 MMAgent::handle_route_message
    │
    ▼
session->enqueue_received_message()
    │ 默认放入 dispatcher 的 intent_request 队列
    │
    ▼
Agent::process_intent_request_loop() / process_data_exchange_loop()
    │ 遍历所有 session，检查 task 类型，分发到具体 task 队列
    │
    ▼
task->enqueue_intent_request_message() 或 enqueue_data_exchange_message()
```

### 设计方案

在 `MMAgent::handle_route_message` 层面处理，检查消息是否携带 `dialogue_id` 和 `task_id`：

**修改位置**：`src/agent/agent.cpp` 的 `handle_route_message` 函数

```cpp
void MMAgent::handle_route_message(const std::shared_ptr<MMAgentSession>& session,
                                   const MMAgentMsgQue::Message& message) {
  // === 新增：检查是否携带 dialogue_id 和 task_id ===
  const auto dialogue_it = message.metadata.find("dialogue_id");
  const auto task_id_it = message.metadata.find("task_id");
  
  if(dialogue_it != message.metadata.end() && task_id_it != message.metadata.end()) {
    const std::string& dialogue_id = dialogue_it->second;
    const std::string& task_id = task_id_it->second;
    
    // 根据 task_id 查找目标 task
    auto target_task = session->get_task(task_id);
    if(target_task && target_task->are_message_threads_running()) {
      // 根据消息类型选择队列
      if(message.type == MMAgentMsgQue::MessageType::DATA_EXCHANGE) {
        target_task->enqueue_data_exchange_message(message);
      } else {
        target_task->enqueue_intent_request_message(message);
      }
      Logger::instance().info_f("[DIRECT_ROUTE] Message routed directly to task=%s, dialogue_id=%s",
                               task_id.c_str(), dialogue_id.c_str());
      return;  // 跳过后续处理
    }
    
    // task 不存在或未运行，降级到默认路由
    Logger::instance().warn_f("[DIRECT_ROUTE] Task not found or not running: %s, falling back to default routing",
                             task_id.c_str());
  }
  
  // === 原有逻辑 ===
  if(message.type == MMAgentMsgQue::MessageType::CONTROL) {
    handle_control_message(session, message);
    return;
  }
  // ... 其余原有代码 ...
}
```

### 消息格式约定

外部消息需要携带以下 metadata 字段：

```json
{
  "session_id": "session-uuid",
  "dialogue_id": "dialogue-123",
  "task_id": "coordinator-0",
  "user_id": "user-001",
  "client_id": "client-001"
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `session_id` | 否 | 用于快速定位 session |
| `dialogue_id` | 是 | 用于 dialogue 隔离 |
| `task_id` | 是 | 目标 task 的 ID |

### 方案优点

| 维度 | 评估 |
|------|------|
| 改动范围 | 小，仅修改一个函数 |
| 架构清晰度 | 高，在消息入口统一处理 |
| 向后兼容 | 完全兼容，无关键字段时走原流程 |
| 使用便捷性 | 自动检测，无需显式调用新 API |
| 性能提升 | 跳过多层遍历，直达目标 task |

### 降级策略

如果定向路由失败（task 不存在或未运行），降级到默认路由流程，保证消息不会丢失。

### 外部获取 task_id 方案

见下文详细分析。

### 实施状态

**设计阶段**：✅ 已完成

**代码变更**：✅ 已完成
- `src/agent/agent.cpp` - 实现消息定向路由逻辑和 task_id 约定命名
- `src/runtime/runtime.cpp` - 注入 `self_task_id` 到 node inputs

**测试验证**：✅ 已完成
- 192 个单元测试全部通过
- 更新测试用例适配新的 task_id 格式

**文档更新**：✅ 已完成
- `publish/INTEGRATION_GUIDE.md` - 新增 7.8 消息定向路由章节
- `publish/workflow/NODE_DEVELOPMENT_GUIDE.md` - 新增 AF_SELF_TASK_ID 说明
- `publish/workflow/NODE_API_REFERENCE.md` - 新增框架注入字段表


### 外部获取 task_id 方案分析

#### 当前 task_id 生成规则

| Task 类型 | task_id 格式 | 示例 |
|-----------|-------------|------|
| Dispatcher | `{session_id}_dispatcher` | `sess-123_dispatcher` |
| Coordinator | `{session_id}_coordinator` | `sess-123_coordinator` |
| Skill Template | `{session_id}_task_{index}` | `sess-123_task_2` |
| Runtime Skill Task | `{template_id}_runtime_{message_id}` | `sess-123_task_3_runtime_skill-travel-abc` |
| Runtime Skill (带 skill_name) | `{template_id}_runtime_{message_id}_{skill_name}` | `sess-123_task_3_runtime_abc_travel` |
| Pooled Task | `{task_id}_pool_{instance_index}` | `sess-123_task_3_pool_0` |

**代码位置**：
- Dispatcher/Coordinator: `src/agent/agent.cpp:681`
- Runtime Task: `src/agent/agent.cpp:1036`
- Skill Runtime: `src/agent/builtin_dispatcher.cpp:235`, `src/agent/agent.cpp:2242`

---

#### 方案一：响应携带 task_id（推荐）

**思路**：在 skill 启动时，将 task_id 放入响应消息返回给外部服务。

**修改位置**：`src/agent/agent.cpp` 的 `launch_skill_task` 函数

```cpp
bool MMAgent::launch_skill_task(...) {
  // ... 创建 skill_task ...
  
  // === 新增：将 task_id 写入 DataBus，供外部查询 ===
  std::string dialogue_id;
  if(context.metadata.count("dialogue_id")) {
    dialogue_id = context.metadata.at("dialogue_id");
  }
  
  if(!dialogue_id.empty()) {
    // 存储当前活跃的 skill task_id 映射
    std::string task_mapping_key = dialogue_id + "/active_skill_tasks/" + skill_name;
    publish_data(task_mapping_key, DataVariant(skill_task->get_task_id()));
    
    // 也可以存储到 session 级别
    std::string session_skill_key = "/session/" + session->get_session_id() 
                                   + "/skill_tasks/" + dialogue_id + "/" + skill_name;
    publish_data(session_skill_key, DataVariant(skill_task->get_task_id()));
  }
  
  // ... 原有逻辑 ...
}
```

**外部服务获取方式**：

```cpp
// 方式1：从 DataBus 读取
std::string task_id_key = dialogue_id + "/active_skill_tasks/" + skill_name;
DataVariant task_id_value;
if(agent->get_data(task_id_key, task_id_value)) {
  std::string task_id = std::get<std::string>(task_id_value);
  // 使用 task_id 发送定向消息
}

// 方式2：从响应消息获取（需要框架在 skill 启动后发送通知）
// 可新增 SKILL_LAUNCHED 事件
```

**优点**：
- ✅ 外部服务可获得精确的 task_id
- ✅ 支持动态 task_id（运行时生成）
- ✅ 可区分同 skill 的多个实例

**缺点**：
- 需要外部服务主动查询或订阅
- 需要维护 task_id 生命周期

---

#### 方案二：DataBus 存储任务映射表

**思路**：在 session 创建时建立 skill_name -> task_id 的映射表，存入 DataBus。

**数据结构**：

```
/session/{session_id}/task_registry/
├── dispatcher = "sess-123_task_0"
├── coordinator = "sess-123_task_1"
├── skills/
│   ├── travel = ["sess-123_task_3_runtime_abc_travel", ...]
│   └── shopping = ["sess-123_task_4_runtime_def_shopping", ...]
└── by_dialogue/
    └── {dialogue_id}/
        ├── active_skills = ["travel", "shopping"]
        └── task_ids = {"travel": "sess-123_task_3_runtime_abc", ...}
```

**实现**：

```cpp
// 在 launch_skill_task 中注册
void register_skill_task(const std::string& session_id,
                        const std::string& dialogue_id,
                        const std::string& skill_name,
                        const std::string& task_id) {
  std::string key = "/session/" + session_id + "/task_registry/by_dialogue/" 
                  + dialogue_id + "/task_ids/" + skill_name;
  publish_data(key, DataVariant(task_id));
  
  // 添加到活跃列表
  std::string active_key = "/session/" + session_id + "/task_registry/by_dialogue/" 
                         + dialogue_id + "/active_skills";
  DataVariant active_value;
  std::vector<std::string> active_skills;
  if(get_data(active_key, active_value)) {
    // 解析并添加
  }
  active_skills.push_back(skill_name);
  // 写回...
}

// skill 完成时注销
void unregister_skill_task(...) {
  // 从 active_skills 移除
}
```

**优点**：
- ✅ 结构化存储，易于查询
- ✅ 支持按 dialogue 隔离
- ✅ 支持多实例管理

**缺点**：
- 实现复杂度较高
- 需要维护生命周期

---

#### 方案三：约定 task_id 命名规则

**思路**：外部服务根据约定规则构造 task_id，无需查询。

**命名规则约定**：

| Task 类型 | task_id 命名规则 |
|-----------|-----------------|
| Dispatcher | `{session_id}_dispatcher` |
| Coordinator | `{session_id}_coordinator` |
| Skill (单实例) | `{session_id}_skill_{skill_name}` |
| Skill (多实例) | `{session_id}_skill_{skill_name}_{instance_index}` |

**修改 task_id 生成逻辑**：

```cpp
// src/agent/agent.cpp:681
// Template task
if(task_definition.task_type == "dispatcher") {
  task_config.task_id = actual_session_id + "_dispatcher";
} else if(task_definition.task_type == "coordinator") {
  task_config.task_id = actual_session_id + "_coordinator";
} else if(task_definition.task_type == "skill") {
  task_config.task_id = actual_session_id + "_skill_" + task_definition.skill_name;
} else {
  task_config.task_id = actual_session_id + "_task_" + std::to_string(task_definition.task_index);
}
```

**外部服务使用**：

```cpp
// 外部服务直接构造 task_id
std::string coordinator_task_id = session_id + "_coordinator";
std::string travel_skill_task_id = session_id + "_skill_travel";

// 发送定向消息
message.metadata["task_id"] = travel_skill_task_id;
message.metadata["dialogue_id"] = dialogue_id;
message.metadata["session_id"] = session_id;
agent->route_request_message(message);
```

**优点**：
- ✅ 实现最简单
- ✅ 外部服务无需查询
- ✅ 无需维护映射表

**缺点**：
- ❌ 需要修改现有 task_id 生成逻辑
- ❌ 多实例场景处理复杂
- ❌ 运行时生成的 task_id（如 runtime task）无法约定

---

#### 方案四：新增查询 API

**思路**：提供 API 让外部服务查询指定 skill 的活跃 task_id。

**API 设计**：

```cpp
// agent.h
class MMAgent {
public:
  /// 查询指定 skill 的活跃 task_id
  /// @param session_id Session ID
  /// @param dialogue_id Dialogue ID（可选）
  /// @param skill_name Skill 名称
  /// @return task_id 列表（可能有多个实例）
  std::vector<std::string> get_active_skill_tasks(
      const std::string& session_id,
      const std::string& dialogue_id,
      const std::string& skill_name) const;
  
  /// 查询 session 中所有活跃 task
  std::map<std::string, std::string> get_active_tasks(
      const std::string& session_id,
      const std::string& dialogue_id = "") const;
};
```

**实现**：

```cpp
std::vector<std::string> MMAgent::get_active_skill_tasks(
    const std::string& session_id,
    const std::string& dialogue_id,
    const std::string& skill_name) const {
  
  auto session = get_session(session_id);
  if(!session) return {};
  
  std::vector<std::string> result;
  auto tasks = session->get_tasks();
  
  for(const auto& task : tasks) {
    if(task->get_task_type() == "skill" &&
       task->get_skill_name() == skill_name &&
       task->are_message_threads_running()) {
      
      // 如果指定了 dialogue_id，需要检查 task 是否属于该 dialogue
      // 可以通过 task parameters 中的 dialogue_id 判断
      std::string task_dialogue_id;
      if(task->get_parameter("dialogue_id", task_dialogue_id)) {
        if(dialogue_id.empty() || task_dialogue_id == dialogue_id) {
          result.push_back(task->get_task_id());
        }
      }
    }
  }
  
  return result;
}
```

**外部服务使用**：

```cpp
// 查询活跃的 travel skill task
auto task_ids = agent->get_active_skill_tasks(session_id, dialogue_id, "travel");
if(!task_ids.empty()) {
  // 使用第一个（或按其他逻辑选择）
  message.metadata["task_id"] = task_ids[0];
  agent->route_request_message(message);
}
```

**优点**：
- ✅ 灵活性高，支持复杂查询
- ✅ 无需修改 task_id 生成逻辑
- ✅ 支持多实例场景

**缺点**：
- 需要额外 API 调用
- 需要外部服务维护调用逻辑

---

#### 方案对比

| 维度 | 方案一 | 方案二 | 方案三 | 方案四 |
|------|--------|--------|--------|--------|
| 实现复杂度 | 低 | 高 | 最低 | 中 |
| 查询开销 | 低（DataBus） | 低（DataBus） | 无 | 中（遍历） |
| 多实例支持 | ✅ | ✅ | ❌ | ✅ |
| 动态 task_id | ✅ | ✅ | ❌ | ✅ |
| 外部服务负担 | 中（需查询） | 中（需查询） | 低（直接构造） | 中（需调用 API） |
| 架构改动 | 小 | 中 | 中 | 小 |

---

#### 推荐组合方案

**方案一 + 方案三组合**：

1. **固定 Task**（dispatcher/coordinator）：采用方案三约定命名
   - `{session_id}_dispatcher`
   - `{session_id}_coordinator`
   
2. **动态 Task**（runtime skill）：采用方案一 DataBus 存储
   - 启动时写入：`{dialogue_id}/active_skill_tasks/{skill_name}`
   - 完成时清理

**实现要点**：

```cpp
// 1. 约定固定 task_id（在 session 创建时）
// src/agent/agent.cpp:681
if(task_definition.task_type == "dispatcher") {
  task_config.task_id = actual_session_id + "_dispatcher";
} else if(task_definition.task_type == "coordinator") {
  task_config.task_id = actual_session_id + "_coordinator";
}

// 2. 动态 skill task_id 写入 DataBus（在 launch_skill_task 时）
std::string task_key = "/session/" + session->get_session_id() 
                      + "/dialogue/" + dialogue_id 
                      + "/skill_tasks/" + skill_name;
publish_data(task_key, DataVariant(skill_task->get_task_id()));
```

**外部服务使用**：

```cpp
// 向 coordinator 发送消息（约定命名）
message.metadata["session_id"] = session_id;
message.metadata["task_id"] = session_id + "_coordinator";
message.metadata["dialogue_id"] = dialogue_id;

// 向运行中的 skill 发送消息（查询 DataBus）
std::string task_key = "/session/" + session_id + "/dialogue/" + dialogue_id 
                      + "/skill_tasks/" + skill_name;
DataVariant task_id_value;
if(agent->get_data(task_key, task_id_value)) {
  message.metadata["task_id"] = std::get<std::string>(task_id_value);
}
```


### 外部获取 task_id 方案

#### 方案：约定命名 + 环境变量注入

**核心思路**：
1. **dispatcher/coordinator**：采用约定的命名方式，外部服务可直接构造 task_id
2. **所有 task**：执行时将自己的 task_id 通过环境变量 `AF_SELF_TASK_ID` 注入到 node input

#### 1. dispatcher/coordinator 约定命名

| Task 类型 | task_id 命名规则 | 示例 |
|-----------|-----------------|------|
| Dispatcher | `{session_id}_dispatcher` | `sess-123_dispatcher` |
| Coordinator | `{session_id}_coordinator` | `sess-123_coordinator` |

**修改位置**：`src/agent/agent.cpp` 的 session 创建逻辑

```cpp
// src/agent/agent.cpp:681 附近
if(task_definition.task_type == "dispatcher") {
  task_config.task_id = actual_session_id + "_dispatcher";
} else if(task_definition.task_type == "coordinator") {
  task_config.task_id = actual_session_id + "_coordinator";
} else {
  task_config.task_id = actual_session_id + "_task_" + std::to_string(task_definition.task_index);
}
```

**外部服务使用**：

```cpp
// 直接构造 task_id，无需查询
message.metadata["session_id"] = session_id;
message.metadata["task_id"] = session_id + "_coordinator";
message.metadata["dialogue_id"] = dialogue_id;
agent->route_request_message(message);
```

#### 2. AF_SELF_TASK_ID 环境变量注入

**思路**：每个 task 执行时，将自己的 task_id 注入到 node inputs，供 node 获取自身身份。

**修改位置**：`src/runtime/runtime.cpp` 的 task poller 输入注入逻辑

```cpp
// src/runtime/runtime.cpp task poller 中
DataMap inputs;

// 现有逻辑
if(task->get_parameter("message_content", msg_content) && !msg_content.empty()) {
  inputs["input"] = msg_content;
  inputs["user_text"] = msg_content;
}

// 新增：注入 self_task_id
inputs["self_task_id"] = task->get_task_id();

// Shell 脚本通过环境变量访问
// AF_SELF_TASK_ID 环境变量会自动设置
```

**环境变量约定**：

| 环境变量 | 值 | 说明 |
|---------|-----|------|
| `AF_SELF_TASK_ID` | `{task_id}` | 当前执行 task 的 ID |

**Node 中使用**：

```bash
# Shell Node
echo "My task_id: $AF_SELF_TASK_ID"

# 将 task_id 传递给外部服务（如需要）
curl -X POST http://external-service/callback \
  -d "{\"task_id\": \"$AF_SELF_TASK_ID\", \"session_id\": \"$AF_SESSION_ID\"}"
```

```python
# Python Node
import os
task_id = os.environ.get("AF_SELF_TASK_ID", "")
print(f"My task_id: {task_id}")
```

```cpp
// C++ SO Node
std::string task_id;
if(inputs.count("self_task_id")) {
  task_id = std::get<std::string>(inputs["self_task_id"]);
}
```

#### 3. 完整数据流

```
外部服务发送消息到 coordinator:
  │
  ├── 构造 task_id = session_id + "_coordinator"
  │
  └── message.metadata["task_id"] = task_id
      message.metadata["dialogue_id"] = dialogue_id
      message.metadata["session_id"] = session_id

框架处理:
  │
  ├── handle_route_message() 检测到 task_id
  │
  └── 直接路由到目标 task 队列

Node 执行:
  │
  ├── inputs["self_task_id"] = task_id
  │
  └── 环境变量 AF_SELF_TASK_ID 可用

Node 通知外部服务:
  │
  └── 输出包含 self_task_id，外部服务保存用于后续定向通信
```

#### 4. 实现要点

**文件修改清单**：

| 文件 | 修改内容 |
|------|---------|
| `src/agent/agent.cpp:681` | dispatcher/coordinator 采用约定命名 |
| `src/runtime/runtime.cpp` | 注入 `self_task_id` 到 node inputs |
| `src/runtime/worker/worker.cpp` | 设置 `AF_SELF_TASK_ID` 环境变量 |

**环境变量注入代码**：

```cpp
// src/runtime/worker/worker.cpp
// 在执行 node 前设置环境变量
std::string task_id = /* 从 execution context 获取 */;
setenv("AF_SELF_TASK_ID", task_id.c_str(), 1);

// 执行 node...

unsetenv("AF_SELF_TASK_ID");  // 清理
```

#### 5. 向后兼容

- skill runtime task 的 task_id 保持原有生成逻辑（带随机性）
- 外部服务如需与 skill 通信，需要从 node 输出中获取 task_id


#### 6. 需要更新的手册内容

##### 6.1 NODE_DEVELOPMENT_GUIDE.md

**文件**：`publish/workflow/NODE_DEVELOPMENT_GUIDE.md`

**更新内容**：

###### 6.1.1 环境变量列表（7.1节）

新增 `AF_SELF_TASK_ID` 环境变量：

```markdown
| 环境变量 | 说明 | 来源 |
|---------|------|------|
| `AF_SELF_TASK_ID` | 当前 task 的 ID | 框架自动设置 |
| `AF_DATABUS_DIR` | DataBus 根目录 | 框架自动设置 |
| ... | ... | ... |
```

###### 6.1.2 环境变量约定（2.1节）

在现有环境变量说明后补充：

```markdown
#### 框架标识变量

以下环境变量由框架自动设置，用于标识当前执行上下文：

| 环境变量 | 说明 | 示例 |
|---------|------|------|
| `AF_SELF_TASK_ID` | 当前 task 的唯一标识 | `sess-123_coordinator` |
| `AF_SESSION_ID` | 当前 session 的唯一标识 | `sess-123` |

**使用示例**：

\`\`\`bash
# Shell Node - 获取自身 task_id
echo "Current task: $AF_SELF_TASK_ID"

# 回调外部服务时携带 task_id
curl -X POST http://external/callback \
  -d "{\"task_id\": \"$AF_SELF_TASK_ID\", \"session_id\": \"$AF_SESSION_ID\"}"
\`\`\`

\`\`\`python
# Python Node - 获取自身 task_id
import os

task_id = os.environ.get("AF_SELF_TASK_ID", "")
session_id = os.environ.get("AF_SESSION_ID", "")

# 保存到输出，供外部服务后续定向通信
outputs["self_task_id"] = task_id
\`\`\`

\`\`\`cpp
// C++ SO Node - 获取自身 task_id
std::string task_id;
if(inputs.count("self_task_id")) {
    task_id = std::get<std::string>(inputs["self_task_id"]);
}
\`\`\`
```

---

##### 6.2 INTEGRATION_GUIDE.md

**文件**：`publish/INTEGRATION_GUIDE.md`

**更新内容**：

###### 6.2.1 新增章节：消息定向路由

建议在 Section 7 后新增：

```markdown
### 7.X 消息定向路由

#### 背景

默认情况下，外部消息会经过 SessionRouter 路由到 session 的 dispatcher 队列，
然后由框架遍历分发到目标 task。

如果外部消息携带了 `dialogue_id` 和 `task_id`，框架会直接将消息发送到目标 task 队列，
跳过中间遍历，实现精确定向。

#### 消息格式

```json
{
  "session_id": "sess-123",
  "dialogue_id": "dialogue-456",
  "task_id": "sess-123_coordinator",
  "user_id": "user-001",
  "client_id": "client-001",
  "content": "消息内容"
}
```

| 字段 | 必填 | 说明 |
|------|------|------|
| `session_id` | 否 | Session ID，用于快速定位 session |
| `dialogue_id` | 是 | Dialogue ID，用于 dialogue 隔离 |
| `task_id` | 是 | 目标 task 的 ID |

#### task_id 获取方式

**dispatcher / coordinator**：约定命名，可直接构造

```cpp
std::string dispatcher_task_id = session_id + "_dispatcher";
std::string coordinator_task_id = session_id + "_coordinator";
```

**runtime skill**：从 node 输出获取

Skill node 执行时可通过环境变量 `AF_SELF_TASK_ID` 获取自身 task_id，
并在输出中返回给外部服务：

```json
{
  "outputs": {
    "self_task_id": "sess-123_task_3_runtime_abc_travel",
    "result": "执行结果"
  }
}
```

#### 使用示例

```cpp
// 外部服务向 coordinator 发送定向消息
MMAgentMsgQue::Message message;
message.message_id = "msg-001";
message.content = "用户输入";
message.metadata["session_id"] = session_id;
message.metadata["dialogue_id"] = dialogue_id;
message.metadata["task_id"] = session_id + "_coordinator";

agent->route_request_message(message);
```
```

---

##### 6.3 NODE_API_REFERENCE.md

**文件**：`publish/workflow/NODE_API_REFERENCE.md`

**更新内容**：

在 node inputs 字段表中新增 `self_task_id`：

```markdown
### 框架注入字段

除了用户定义的输入参数，框架还会自动注入以下字段：

| 字段名 | 类型 | 说明 |
|--------|------|------|
| `self_task_id` | string | 当前 task 的唯一标识 |
| `session_id` | string | 当前 session 的唯一标识 |
| `dialogue_id` | string | 当前 dialogue 的唯一标识（如适用） |
| `exec_skill_key` | string | skill 专属数据源 key（如配置） |
```

---

##### 6.4 更新清单汇总

| 文件 | 更新类型 | 更新内容 |
|------|---------|---------|
| `NODE_DEVELOPMENT_GUIDE.md` | 新增 | 环境变量 `AF_SELF_TASK_ID` 说明及示例 |
| `NODE_DEVELOPMENT_GUIDE.md` | 修改 | 7.1 环境变量完整列表 |
| `INTEGRATION_GUIDE.md` | 新增 | 7.X 消息定向路由章节 |
| `NODE_API_REFERENCE.md` | 新增 | 框架注入字段表（`self_task_id`） |


---
