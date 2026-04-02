# Session 老化与 DataBus 清理机制 Enhancement

## 1. 背景

Session 老化超时后归还到池，但 **不会清理 DataBus 中 `/session/{session-id}/` 下的数据**，导致脏数据、内存泄漏和数据隔离失效问题。

## 2. 方案简述

### 问题拆解

1. **自动清理**：Session 老化时自动清理 DataBus 数据
2. **手动清理**：提供 API 支持外部主动清理
3. **多级清理**：支持 Session 级别和 Dialogue 级别清理

### 架构设计

```
┌─────────────────────────────────────────────────────────────┐
│                      MMAgentRuntime                         │
├─────────────────────────────────────────────────────────────┤
│  clear_session_data(agent_id, session_id)                   │
│  clear_dialogue_data(agent_id, session_id, dialogue_id)     │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                        MMAgent                               │
├─────────────────────────────────────────────────────────────┤
│  clear_session_data(session_id)                             │
│  clear_dialogue_data(session_id, dialogue_id)               │
│                                                              │
│  Implementation:                                             │
│  - runtime_databus_->remove_by_prefix("/session/{id}/")     │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                       MMDataBus                              │
├─────────────────────────────────────────────────────────────┤
│  remove_by_prefix(prefix)  ← 已实现，有线程安全保护         │
│  ├── ZeroCopyDataBus::remove_by_prefix()                    │
│  │   └── unique_lock<shared_mutex> lock(data_mutex_)        │
│  └── SharedMemoryBlock::remove_by_prefix()                  │
└─────────────────────────────────────────────────────────────┘
```

## 3. 约束与假设

### 生效场景
- 仅 Master Runtime 有 DataBus 访问权限
- Worker Runtime 无直接 DataBus 访问（清理调用无效）

### 依赖关系
- 依赖 `remove_by_prefix` 批量删除接口
- 依赖 `runtime_databus_` 指针（由 Runtime 注入）

### 兼容性
- 新增 API，不影响现有功能
- 老化自动清理为可选行为

## 4. Session 老化超时机制分析

### 触发流程

```
aging_loop (每 aging_interval_ms 检查一次)
    │
    ▼
release_expired_sessions()
    │
    ├── 遍历 active_routes_
    │   └── session->is_timeout_expired() 判断超时
    │       └── (current_time - last_activity_time_) > timeout_ms
    │
    └── release_route_locked(route_key)
        ├── session_to_route_.erase(session_id)
        ├── session->reset_for_pool()  ← 仅清理 session 自身状态
        └── active_routes_.erase(route_key)
```

### 关键代码位置

| 组件 | 文件 | 行号 |
|------|------|------|
| 老化线程 | `session_router.cpp` | 284-292 |
| 超时判断 | `session.cpp` | 207-214 |
| 归还池 | `session.cpp` | 147-180 |
| 路由释放 | `session_router.cpp` | 257-267 |

### 当前行为问题

```cpp
// session_router.cpp:257-267
void SessionRouter::release_route_locked(const std::string& route_key) {
  const auto it = active_routes_.find(route_key);
  if(it == active_routes_.end()) return;
  if(it->second) {
    session_to_route_.erase(it->second->get_session_id());
    it->second->reset_for_pool();  // ← 只清理 session 对象，不清理 DataBus
  }
  active_routes_.erase(it);
}
```

### reset_for_pool 清理内容

```cpp
// session.cpp:147-180
void MMAgentSession::reset_for_pool() {
  state_ = SessionState::CREATED;
  active_route_key_.clear();
  active_user_id_.clear();
  active_client_id_.clear();
  metadata_.clear();
  runtime_tasks_.clear();
  // ... 清理 session 对象内部状态
  // ❌ 不清理 DataBus 数据
}
```

## 5. 数据结构设计

### 清理范围

| 清理类型 | Key 前缀 | 清理内容 |
|---------|---------|---------|
| Session | `/session/{session_id}/` | 整个 session 下所有数据 |
| Dialogue | `/session/{session_id}/sys/{dialogue_id}/` | 单个 dialogue 的框架数据 |
| Dialogue | `/session/{session_id}/user/{dialogue_id}/` | 单个 dialogue 的用户数据 |
| Dialogue | `/session/{session_id}/ctx/{dialogue_id}/` | 单个 dialogue 的上下文数据 |

### API 设计

```cpp
// MMAgent 层接口
class MMAgent {
public:
  // 清理整个 session 的 DataBus 数据
  int clear_session_data(const std::string& session_id);

  // 清理单个 dialogue 的 DataBus 数据
  int clear_dialogue_data(const std::string& session_id,
                           const std::string& dialogue_id);
};

// MMAgentRuntime 层接口
class MMAgentRuntime {
public:
  // 清理指定 agent 的 session 数据
  int clear_session_data(const AgentId& agent_id,
                         const std::string& session_id);

  // 清理指定 agent 的 dialogue 数据
  int clear_dialogue_data(const AgentId& agent_id,
                           const std::string& session_id,
                           const std::string& dialogue_id);
};
```

## 6. 实现细节

### MMAgent::clear_session_data

```cpp
int MMAgent::clear_session_data(const std::string& session_id) {
  if(!runtime_databus_) {
    Logger::instance().warn_f("[MMAgent] No runtime databus for session cleanup");
    return 0;
  }

  std::string prefix = "/session/" + session_id + "/";
  int count = runtime_databus_->remove_by_prefix(prefix);

  Logger::instance().info_f("[MMAgent] Cleared %d keys for session_id=%s",
                            count, session_id.c_str());
  return count;
}
```

### MMAgent::clear_dialogue_data

```cpp
int MMAgent::clear_dialogue_data(const std::string& session_id,
                                  const std::string& dialogue_id) {
  if(!runtime_databus_) {
    Logger::instance().warn_f("[MMAgent] No runtime databus for dialogue cleanup");
    return 0;
  }

  int total_count = 0;

  // Clean sys namespace
  std::string sys_prefix = "/session/" + session_id + "/sys/" + dialogue_id + "/";
  total_count += runtime_databus_->remove_by_prefix(sys_prefix);

  // Clean user namespace
  std::string user_prefix = "/session/" + session_id + "/user/" + dialogue_id + "/";
  total_count += runtime_databus_->remove_by_prefix(user_prefix);

  // Clean ctx namespace
  std::string ctx_prefix = "/session/" + session_id + "/ctx/" + dialogue_id + "/";
  total_count += runtime_databus_->remove_by_prefix(ctx_prefix);

  Logger::instance().info_f("[MMAgent] Cleared %d keys for session_id=%s, dialogue_id=%s",
                            total_count, session_id.c_str(), dialogue_id.c_str());
  return total_count;
}
```

### MMAgentRuntime 封装

```cpp
int MMAgentRuntime::clear_session_data(const AgentId& agent_id,
                                        const std::string& session_id) {
  auto agent = get_agent(agent_id);
  if(!agent) {
    Logger::instance().warn_f("[Runtime] Agent not found: %s", agent_id.c_str());
    return 0;
  }
  return agent->clear_session_data(session_id);
}

int MMAgentRuntime::clear_dialogue_data(const AgentId& agent_id,
                                          const std::string& session_id,
                                          const std::string& dialogue_id) {
  auto agent = get_agent(agent_id);
  if(!agent) {
    Logger::instance().warn_f("[Runtime] Agent not found: %s", agent_id.c_str());
    return 0;
  }
  return agent->clear_dialogue_data(session_id, dialogue_id);
}
```

## 7. 自动清理集成（可选）

### 在 SessionRouter 中集成

```cpp
void SessionRouter::release_route_locked(const std::string& route_key) {
  const auto it = active_routes_.find(route_key);
  if(it == active_routes_.end()) return;
  if(it->second) {
    // 新增：自动清理 DataBus 数据
    std::string session_id = it->second->get_session_id();
    if(databus_) {  // 需要注入 databus_ 指针
      std::string prefix = "/session/" + session_id + "/";
      databus_->remove_by_prefix(prefix);
    }

    session_to_route_.erase(session_id);
    it->second->reset_for_pool();
  }
  active_routes_.erase(it);
}
```

## 8. 测试方案

### 单元测试

```cpp
TEST(DataBusCleanup, ClearSessionData) {
  MMDataBus databus(1024 * 1024);
  MMAgent agent(Config{.id = "test-agent"});
  agent.set_runtime_databus(&databus);

  // Put test data
  databus.put("/session/s1/sys/key1", DataVariant("value1"));
  databus.put("/session/s1/user/key2", DataVariant("value2"));
  databus.put("/session/s2/sys/key3", DataVariant("value3"));

  // Clear session s1
  int count = agent.clear_session_data("s1");

  EXPECT_EQ(count, 2);  // 2 keys removed
  EXPECT_FALSE(databus.contains("/session/s1/sys/key1"));
  EXPECT_FALSE(databus.contains("/session/s1/user/key2"));
  EXPECT_TRUE(databus.contains("/session/s2/sys/key3"));  // s2 unchanged
}

TEST(DataBusCleanup, ClearDialogueData) {
  MMDataBus databus(1024 * 1024);
  MMAgent agent(Config{.id = "test-agent"});
  agent.set_runtime_databus(&databus);

  // Put test data
  databus.put("/session/s1/sys/d1/key1", DataVariant("value1"));
  databus.put("/session/s1/sys/d2/key2", DataVariant("value2"));
  databus.put("/session/s1/user/d1/key3", DataVariant("value3"));

  // Clear dialogue d1
  int count = agent.clear_dialogue_data("s1", "d1");

  EXPECT_EQ(count, 2);  // 2 keys removed (sys/d1 and user/d1)
  EXPECT_FALSE(databus.contains("/session/s1/sys/d1/key1"));
  EXPECT_TRUE(databus.contains("/session/s1/sys/d2/key2"));  // d2 unchanged
}
```

## 9. 实施状态

| 阶段 | 状态 | 说明 |
|------|------|------|
| 问题识别 | ✅ 已完成 | 确认 DataBus 数据残留问题 |
| remove_by_prefix 实现 | ✅ 已完成 | ZeroCopyDataBus 已实现 |
| MMAgent 清理函数 | ✅ 已完成 | clear_session_data, clear_dialogue_data |
| Runtime 封装 | ✅ 已完成 | 封装到 MMAgentRuntime |
| 自动清理集成 | ⏳ 待定 | SessionRouter 老化时自动清理（可选） |
| 单元测试 | ✅ 已完成 | 6个测试用例验证清理功能 |

## 10. 相关文件

| 文件 | 修改类型 |
|------|---------|
| `include/internal/agent/agent.h` | 新增 clear_session_data, clear_dialogue_data |
| `src/agent/agent.cpp` | 实现清理函数 |
| `include/internal/runtime/agent_runtime.h` | 新增 Runtime 层封装 |
| `src/runtime/runtime.cpp` | 实现 Runtime 层封装 |
| `tests/unit/src/test_agent_gtest.cpp` | 新增清理功能测试 |

## 11. 文档

### 需要变更的文档

| 文档 | 变更说明 |
|------|---------|
| [docs/api-reference.md](../api-reference.md) | 在 MMAgent API 和 MMAgentRuntime API 章节添加 clear_session_data/clear_dialogue_data 接口说明 |
| [docs/databus-session-isolation.md](../databus-session-isolation.md) | 在"Implementation Details"章节添加 DataBus 清理机制说明，包括清理时机和清理范围 |

### 关联方案

| 关联方案 | 关系说明 |
|---------|---------|
| [003-MMDataBus 全局共享重构](003-refactor-mmdatabus-全局共享重构-260222-implemented.md) | DataBus 基础架构，remove_by_prefix 接口依赖 |
| [021-Workflow Input Dialogue 级别隔离](021-feature-workflow-input-dialogue-级别隔离-260318-implemented.md) | Dialogue 级别数据隔离设计，清理范围定义依赖 |

## 12. 进展跟踪

- [x] 代码实现
- [x] 单元测试
- [ ] 集成测试（可选）
- [ ] 文档更新（api-reference.md, databus-session-isolation.md）
- [x] 验证通过