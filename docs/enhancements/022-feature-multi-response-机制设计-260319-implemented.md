## 2026-03-19 Multi-Response 机制设计

### 背景

当前框架支持 coordinator 对单个 dialogue 返回单条响应（`response` key）。
但在流式输出、渐进式结果推送等场景下，coordinator 需要分多次返回多条响应。

### DataBus Key 结构

```
/session/{session-id}/sys/{dialogue-id}/
├── response                    # 单响应（保持兼容）
├── responses/                  # 多响应中间节点
│   ├── total_count = N         # 当前响应总数（下一个写入 index）
│   ├── consumed_count = M      # 已消费响应数（下一个消费 index）
│   ├── done = true/false       # 会话是否完成
│   ├── 0/
│   │   ├── content = "响应1"   # coordinator 写入
│   │   └── consumed = false    # 框架标记
│   └── ...
```

### 消息流转接口变更

**Coordinator Node 输出**：

| 输出 Key | 格式 | 说明 |
|---------|------|------|
| `response` | string | 单响应（向后兼容） |
| `responses` | JSON | 多响应：`{"content": "...", "done": false}` |

**框架发布**（runtime.cpp）：

1. 检测 node 输出 `responses` key
2. 读取 `responses/total_count` 作为新 index
3. 写入 `responses/{index}/content` 和 `consumed=false`
4. 递增 `total_count`
5. 若 `done=true`，写入 `responses/done=true`

**框架消费**（agent.cpp）：

1. 读取 `responses/consumed_count` 定位下一个消费 index
2. 读取 `responses/{consumed_count}/content`
3. 发送给客户端
4. 标记 `consumed=true`，递增 `consumed_count`
5. 若所有已消费且 `done=true`，清理 dialogue 数据

### 向后兼容

- 单响应 `response` 继续工作
- 多响应 `responses` 为新增功能
- 两者可同时使用，优先处理多响应

### 源文件变更

| 文件 | 变更内容 |
|------|----------|
| `src/runtime/runtime.cpp` | 新增多响应发布逻辑：检测 `responses` key，写入带 index 的结构 |
| `src/agent/agent.cpp` | 新增多响应消费逻辑：`consume_multi_response`、`mark_response_consumed`、`cleanup_multi_responses` 辅助函数；修改 coordinator data_exchange 处理 |

### 实施状态

**设计阶段**：✅ 已完成

**代码变更**：✅ 已完成

**文档更新**：✅ 已完成
- `publish/INTEGRATION_GUIDE.md` - 新增 Section 7.6 Multi-Response 流式输出

**测试验证**：✅ 已完成
- `tests/unit/src/test_multi_response_gtest.cpp` - 单元测试通过
- 所有 191 个 GoogleTest 测试通过

---
