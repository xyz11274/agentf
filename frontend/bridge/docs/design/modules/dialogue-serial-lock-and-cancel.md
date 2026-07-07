# Session 级 Dialogue 串行锁与取消机制设计

**层级**: L4 技术设计（`docs/design/modules/`）
**状态**: 已固化
**日期**: 2026-07-04
**关联**: stale WORKFLOW_COMPLETE 修复（2026-07-04）、跨对话污染 E2E 发现

---

## 1. 背景与问题

### 1.1 问题现象

E2E 测试 `test_stale_workflow_completion_e2e.cpp` Test 3 发现：同一 `(user_id, client_id)` 顺序发送两个 dialogue A、B，B 的响应包含 A 的查询内容（跨对话污染）。

### 1.2 根因

session 内 `coordinator_task_` / `dispatcher_task_` 是**单例**，其内部状态被所有 dialogue 共享：

| 共享成员 | 污染机制 |
|---------|---------|
| `parameters_["message_content"]` | 后到的 dialogue 覆盖先到的；先启动的 workflow 读到后者的值 |
| `data_exchange_messages_` | 多 dialogue 消息混队 |
| `intent_request_messages_` | 多 dialogue trigger 消息混队 |
| `response_messages_` | 多 dialogue 响应混队 |
| `running_skills_count_` | 多 dialogue 计数累加 |

DataBus 层已用 `dialogue_id + "/"` 前缀隔离，但 Task 层未隔离——半迁移状态。

### 1.3 业界对比

| 框架 | 隔离单位 | 并发模型 |
|------|---------|---------|
| OpenAI Assistants | Thread | 同 Thread 串行 |
| LangGraph | Thread | 同 Thread 串行（state lock） |
| AutoGen | Conversation | 同 conversation 串行 |

**业界共识**：对话是有序消息流，同对话内必须串行。并发只在不同对话之间发生。

### 1.4 当前并发入口

| 入口 | 并发控制 | 同 session 并发？ |
|------|---------|-----------------|
| WebSocket `/ws/chat` | ✅ Bridge 显式拒绝并发 turn | ❌ |
| HTTP `/api/tasks`（上游 SysAgent） | ❌ `asyncio.create_task` 立即 202 | ✅ **会发生** |

上游 SysAgent 可对同一 `(userName, deviceId)` 快速连发多个 TaskAssignment。

---

## 2. 设计目标

1. **同 session 内 dialogue 串行**：同一 `(user_id, client_id)` 的多个 dialogue 必须排队执行，不并发
2. **并发现象支持**：同一用户用同一 client_id 同时发消息，请求被接受并排队，不拒绝
3. **可手动中断**：用户/上游可中断正在执行的 dialogue，释放串行锁，让队列中下一个 dialogue 执行
4. **锁释放可靠**：无论 dialogue 完成、失败、取消、超时，锁必须释放，不死锁

---

## 3. 设计方案

### 3.1 核心机制：Dialogue 串行锁

在 `MMAgentSession` 增加 dialogue 级串行锁：

```cpp
class MMAgentSession {
 public:
  /// 获取 dialogue 串行锁（阻塞直到前一个 dialogue 释放）。
  /// 返回 RAII guard，析构时释放锁。
  /// 取消令牌：若 wait 期间本 session 被取消，立即返回 nullptr。
  std::unique_ptr<DialogueLockGuard> acquire_dialogue_lock(
      const std::string& dialogue_id,
      std::atomic<bool>& cancel_token);

 private:
  std::mutex dialogue_serialize_mutex_;
  std::condition_variable dialogue_serialize_cv_;
  std::string active_dialogue_id_;  // 当前持锁的 dialogue_id（空=空闲）
};
```

**RAII Guard**：

```cpp
class DialogueLockGuard {
 public:
  DialogueLockGuard(MMAgentSession& session, const std::string& dialogue_id);
  ~DialogueLockGuard();  // 释放锁，notify cv
  bool is_cancelled() const;
 private:
  MMAgentSession& session_;
  std::string dialogue_id_;
};
```

### 3.2 加锁位置

`MMAgent::handle_intent_request`（agent.cpp L1248 附近），在生成 dialogue_id 之后、写入 DataBus 之前：

```cpp
// 1. 生成 dialogue_id
// 2. 获取串行锁（阻塞，支持取消）
auto lock = session->acquire_dialogue_lock(dialogue_id, session_cancel_token);
if (!lock || lock->is_cancelled()) {
  // session 被销毁或取消，拒绝请求
  return;
}
// 3. 写入 DataBus、触发 workflow（持锁期间执行）
// 4. workflow 完成/失败/取消后，lock 析构释放
```

**持锁范围**：从 `handle_intent_request` 进入到 dialogue 响应发送完成（`cleanup_dialogue_data` 调用后）。

### 3.3 锁释放时机（关键）

锁在以下任一情况释放：

| 时机 | 触发点 | 机制 |
|------|--------|------|
| dialogue 正常完成 | `cleanup_dialogue_data` 后 | lock 对象析构（RAII） |
| dialogue 失败 | 异常处理路径 | lock 对象析构（栈展开） |
| dialogue 被取消 | `handle_dialogue_interrupt` | 设置 cancel_token + interrupt skill tasks |
| session 销毁 | `handle_session_destroy` | 设置 session_cancel_token，唤醒所有 wait |

**关键不变量 I-1**：lock 是栈对象，无论正常返回还是异常抛出，析构必然执行 → 锁必然释放。

**关键不变量 I-2**：`acquire_dialogue_lock` 的 wait 同时监听 `session_cancel_token`，session 销毁时立即唤醒所有等待者。

### 3.4 Dialogue 取消机制

#### 3.4.1 新增控制命令

```cpp
// msgque.h
struct ControlCommand {
  static constexpr const char* SESSION_DESTROY    = "SESSION-DESTROY";
  static constexpr const char* SESSION_INTERRUPT  = "SESSION-INTERRUPT";
  static constexpr const char* DIALOGUE_INTERRUPT = "DIALOGUE-INTERRUPT";  // 新增
};
```

#### 3.4.2 daemon JSON-RPC 方法

新增 `dialogue.interrupt` 方法：

```json
{
  "jsonrpc": "2.0",
  "method": "dialogue.interrupt",
  "params": {
    "agent_id": "outfit-assistant",
    "session_id": "ses_xxx",      // 可选，按 route_key 解析
    "dialogue_id": "d_xxx"
  },
  "id": 1
}
```

#### 3.4.3 handle_dialogue_interrupt 流程

```cpp
void MMAgent::handle_dialogue_interrupt(session, dialogue_id) {
  // 1. 中断该 dialogue 关联的所有 skill tasks
  for (auto& task : session->get_tasks()) {
    std::string task_did;
    if (task->get_parameter("dialogue_id", task_did) && task_did == dialogue_id) {
      task->interrupt();
      task->stop_message_threads();
    }
  }
  // 2. 通知 coordinator 该 dialogue 被取消
  //    → coordinator 走 cleanup 路径 → lock 析构释放
  // 3. 清理 DataBus 中该 dialogue 的数据
  BuiltinDispatcher::cleanup_dialogue_data(*this, session_id, dialogue_id);
}
```

**关键**：取消不直接释放锁，而是让 dialogue 走正常 cleanup 路径，由 RAII 析构释放——保证锁释放与数据清理原子。

### 3.5 Bridge 联动

#### 3.5.1 WebSocket stop 帧联动

当前 Bridge `stop_event` 只 abort 轮询，不下发 daemon。改为：

```python
# chat.py _run_turn
if stop_event.is_set():
    # 下发取消到 daemon
    await daemon_client.send_request("dialogue.interrupt", {
        "agent_id": agent_id,
        "dialogue_id": dialogue_id,
    })
    break
```

#### 3.5.2 aisf_tasks 并发排队

当前 `aisf_tasks.py` L126 `asyncio.create_task` 立即并发执行。改为：**保持并发接受（立即 202），但 daemon 侧串行化**。无需 Bridge 改动——daemon 的串行锁自然让后到的 dialogue 排队。

---

## 4. 不变量

| ID | 不变量 | 验证方式 |
|----|--------|---------|
| I-1 | 同一 session 内任意时刻最多 1 个 dialogue 持锁执行 | 单元测试 + E2E |
| I-2 | lock 析构必然释放锁（RAII，异常安全） | 单元测试（异常路径） |
| I-3 | session 销毁时所有等待锁的 dialogue 立即被唤醒并拒绝 | 单元测试 |
| I-4 | dialogue 取消后，队列中下一个 dialogue 在锁释放后立即执行 | E2E |
| I-5 | 取消不破坏 DataBus 隔离（cleanup 后无残留） | 单元测试 |

---

## 5. 改动清单

### 5.1 C++ Daemon

| 文件 | 改动 |
|------|------|
| `include/internal/agent/session.h` | 新增 `DialogueLockGuard` 类、`acquire_dialogue_lock`、`dialogue_serialize_mutex_`、`active_dialogue_id_`、`session_cancel_token_` |
| `src/agent/session.cpp` | 实现加锁/解锁逻辑 |
| `include/internal/agent/msgque.h` | 新增 `ControlCommand::DIALOGUE_INTERRUPT` |
| `src/agent/agent.cpp` | `handle_intent_request` 加锁；新增 `handle_dialogue_interrupt`；control message 路由新增 `DIALOGUE-INTERRUPT` 分支 |
| `src/daemon/` (JSON-RPC) | 新增 `dialogue.interrupt` 方法 |

### 5.2 Python Bridge

| 文件 | 改动 |
|------|------|
| `frontend/bridge/routes/chat.py` | `_run_turn` stop 时下发 `dialogue.interrupt` |
| `frontend/bridge/client.py` | 新增 `dialogue_interrupt` 方法封装 |

### 5.3 测试

| 测试 | 覆盖 |
|------|------|
| C++ 单元 `test_agent_gtest` | I-1/I-2/I-3/I-5（mock session + 多线程加锁） |
| E2E `test_stale_workflow_completion_e2e` Test 3 | 改为验证串行隔离（B 等 A 完成后执行，拿到自己的响应） |
| E2E 新增 `test_dialogue_cancel_e2e` | I-4（取消 A 后 B 立即执行） |

---

## 6. 不做的事

- **不做 per-dialogue task 实例化**（方向 B）：重构量大，且串行模型下无必要
- **不移除 task parameters 共享**（方向 C）：串行后共享无竞态
- **不加 Bridge 层并发队列**：daemon 侧串行已足够，Bridge 保持立即接受

---

## 7. 验收标准

1. 同 session 顺序两 dialogue：B 等 A 完成后执行，B 响应不含 A 的内容
2. 同 session 并发两 dialogue：都返回 202/接受，但执行串行，各自拿到自己的响应
3. 取消正在执行的 dialogue：队列中下一个立即执行
4. session 销毁：所有等待的 dialogue 立即被拒绝，不死锁
5. 全量回归：C++ 61+ 单元、Bridge 260+ 单元、E2E 全通过
