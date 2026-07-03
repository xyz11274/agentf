## 2026-03-18 Workflow Input Dialogue 级别隔离

### 背景

当多个 dialogue 在同一 session 内并行执行时（如多轮对话、并发 skill 任务），workflow inputs 存储在 agent DataBus 中没有 dialogue_id 前缀，导致不同 dialogue 的输入数据可能相互覆盖。

### 问题分析

**第三层映射（Workflow Inputs → Node Inputs）**：
- `execute_workflow()` 将 inputs 存入 agent DataBus
- `process_node()` 从 agent DataBus 读取 inputs 注入节点
- 原实现：`agent->publish_data(key, value)` 无前缀
- 并发场景：dialogue A 的 `user_text` 可能被 dialogue B 的 `user_text` 覆盖

### 解决方案

在 workflow inputs 存取时增加 `{dialogue_id}/` 前缀：

| 操作 | 原实现 | 新实现 |
|------|--------|--------|
| 存储 | `key` | `{dialogue_id}/key` |
| 读取 | `key` | `{dialogue_id}/key`，失败则 fallback 到 `key` |

**代码变更**（`src/runtime/runtime.cpp`）：

```cpp
// execute_workflow(): 存储时添加前缀
std::string input_prefix = dialogue_id.empty() ? "" : dialogue_id + "/";
for(const auto& pair : inputs) {
  agent->publish_data(input_prefix + pair.first, pair.second);
}

// process_node(): 读取时添加前缀，失败则 fallback
std::string input_prefix = workflow_dialogue_id.empty() ? "" : workflow_dialogue_id + "/";
if(agent->get_data(input_prefix + key, val)) {
  hydrated[key] = val;
} else if(agent->get_data(key, val)) {
  // Fallback: backward compatibility
  hydrated[key] = val;
}
```

### 向后兼容

- `dialogue_id` 为空时不添加前缀
- 读取时 fallback 到无前缀键
- 现有代码无需修改

### 文档更新

- `docs/databus-session-isolation.md`：新增 Dialogue-Level Isolation 章节

### 验证结果

- `./build.sh build`：通过
- `./build.sh test`：19/19 测试通过
