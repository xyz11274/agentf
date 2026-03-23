## 2026-03-20 Session 老化 DataBus 清理问题

### 问题描述

Session 老化超时后归还到池，但 **不会清理 DataBus 中 `/session/{session-id}/` 下的数据**。

### 当前行为

| 触发方式 | Session 状态 | DataBus 数据 |
|---------|-------------|--------------|
| 老化超时 | reset_for_pool (归还池) | ❌ **不清理** |
| SESSION-DESTROY | 销毁 | ✅ 清理 `/session/{id}/*` |

### 代码位置

`src/agent/session_router.cpp:257-267`

```cpp
void SessionRouter::release_route_locked(const std::string& route_key) {
  const auto it = active_routes_.find(route_key);
  if(it == active_routes_.end()) {
    return;
  }
  if(it->second) {
    session_to_route_.erase(it->second->get_session_id());
    it->second->reset_for_pool();  // 只清理 session 自身状态
  }
  active_routes_.erase(it);
}
```

### 问题影响

1. **脏数据**：Session 复用时可能读到上次残留数据
2. **内存泄漏**：DataBus 数据持续累积
3. **数据隔离失效**：不同用户的 session 数据可能混淆

### 建议修复

```cpp
void SessionRouter::release_route_locked(const std::string& route_key) {
  const auto it = active_routes_.find(route_key);
  if(it == active_routes_.end()) {
    return;
  }
  if(it->second) {
    // 新增：清理 DataBus 数据
    if(data_bus_) {
      std::string prefix = "/session/" + it->second->get_session_id();
      data_bus_->remove_by_prefix(prefix);
    }
    
    session_to_route_.erase(it->second->get_session_id());
    it->second->reset_for_pool();
  }
  active_routes_.erase(it);
}
```

### 实施状态

**问题识别**：✅ 已确认

**代码修复**：待实施


---
