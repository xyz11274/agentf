## 2026-03-19 next_skills 格式扩展

### 背景

原 `next_skills` 格式为简单字符串数组，无法为每个 skill 指定专属数据源。
扩展格式支持 `exec_skill_key` 字段，实现 skill 输入数据的精准分发。

### 格式对比

**旧格式（向后兼容）**：
```json
["skill1", "skill2"]
```

**新格式**：
```json
[
  {"name": "skill1", "exec_skill_key": "key1-in-databus"},
  {"name": "skill2", "exec_skill_key": "key2-in-databus"}
]
```

### 数据结构

```cpp
struct SkillSpec {
  std::string name;            // skill 名称
  std::string exec_skill_key;  // 可选：DataBus key
};
```

### 数据流

1. Dispatcher 将解析结果存入 DataBus 不同 key
2. `parse_next_skills()` 解析新格式，返回 `SkillSpec` 列表
3. `launch_skill_task()` 将 `exec_skill_key` 写入消息 metadata 和 task 参数
4. `runtime.cpp` 将 `exec_skill_key` 注入到 node inputs
5. Node 通过 `inputs["exec_skill_key"]` 读取专属数据

### 向后兼容

- 旧格式字符串数组继续工作
- `exec_skill_key` 为可选字段，默认为空

### 源文件变更

| 文件 | 变更内容 |
|------|----------|
| `include/internal/agent/builtin_dispatcher.h` | 新增 `SkillSpec` 结构体，更新 `launch_skill_task()` 和 `parse_next_skills()` 声明 |
| `include/internal/agent/agent.h` | 更新 `launch_skill_task()` 声明添加 `exec_skill_key` 参数 |
| `src/agent/builtin_dispatcher.cpp` | 修改 `parse_next_skills()` 返回 `SkillSpec` 列表，支持新格式解析；修改 `launch_skill_task()` 实现 |
| `src/agent/agent.cpp` | 更新 `launch_skill_task()` 实现，添加 `exec_skill_key` 到消息和任务参数；更新所有调用点 |
| `src/runtime/runtime.cpp` | 在 task poller 中注入 `exec_skill_key` 到 node inputs |

### 实施状态

**设计阶段**：✅ 已完成

**代码变更**：✅ 已完成

**测试验证**：✅ 已完成
- `tests/unit/src/test_agent_gtest.cpp` - 新增 `BuiltinDispatcherParsesNextSkillsWithExecSkillKey` 测试
- 更新现有测试使用 `.name` 访问器
- 所有相关单元测试通过

---
