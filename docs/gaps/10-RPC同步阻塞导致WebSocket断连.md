# Gap 10: RPC 同步阻塞导致 WebSocket 断连

## 状态
⚠️ **未实现 — 架构缺陷**

## 问题描述

长时间 RPC 操作（如 `agent.load`）在 WebSocket 的 **client_handler 线程中同步执行**，导致该线程无法处理 WebSocket 控制帧（ping/pong/close），连接因心跳超时被断开。

## 现象

### 桥接器日志
```
[WARNING] routes.daemon: daemon health probe failed: timeout waiting for response to 'health' after 5.0s
[WARNING] routes.agents: daemon proxy error: timeout waiting for response to 'agent.list' after 5.0s -> HTTP 504
[INFO] client: daemon websocket closed: received 1005 (no status received [internal])
[INFO] client: reconnecting in 1.0s (backoff=1.0)
```

### 直接测试（Python websockets）
```
Sent agent.load at t=..., waiting up to 60s...
→ 20 秒后 websockets 库发出 keepalive ping → daemon 线程卡死无 pong 回复
→ 40 秒后连接断开: "sent 1011 (internal error) keepalive ping timeout"
```

### Daemon 状态
- daemon 进程存活，TCP 端口 LISTEN
- 能接受新的 WebSocket 连接（`WS_UPGRADE` 成功）
- 但已有连接上的 RPC handler 线程**永久阻塞**，不返回响应
- 短时间内建立的新连接可以正常处理 `health` 等快速请求

## 代码分析

### 同步阻塞路径（`rpc_server.cpp:442-471`）

```cpp
// client_handler 线程 — 单线程处理所有 WebSocket 帧
while (ws_decode_frame(recv_buffer, payload, opcode)) {
    if (opcode == WS_OPCODE_CLOSE)  { ... goto cleanup; }
    if (opcode == WS_OPCODE_PING)   { ... continue; }   // ← ping 处理
    if (opcode == WS_OPCODE_TEXT) {
        std::string response = handle_request(payload, fd, channel); // ← 同步阻塞！
        // 直到 handle_request 返回才能处理下一个帧
    }
}
```

### `agent.load` 调用链（`rpc_handler.cpp:250-267`）

```
handle_agent_load()
  ├─ registry_.load_agent()          ← AgentRegistry::mutex_ 加锁
  │   ├── AgentPackage::load_from_directory()
  │   ├── MMAgent::create_from_package()   ← 解析 package.json + 所有 workflow
  │   ├── agent->initialize()              ← 初始化组件
  │   └── runtime.register_agent()         ← 注册到运行时 + 部署 Workflow
  └─ entry.agent->start()                 ← 启动 Session、消息线程
```

上述整个调用链在 client_handler 线程中**同步执行**，期间：
1. **无法从 socket 读取数据**（`recv` 不会被调用）
2. **无法响应 WebSocket ping 帧**（poll → recv 循环被阻塞）
3. 客户端 websockets 库的 keepalive 机制发送 ping，20 秒无 pong→断开连接

### Daemon 初始化顺利的原因

daemon 启动时通过 `--package` 加载 agent 是在**主线程**中完成的，不涉及 WebSocket 连接，所以没有 ping/pong 问题。而 `agent.load` RPC 是在 WebSocket client_handler 线程中触发，才暴露了此问题。

## 影响

| 场景 | 影响 |
|------|------|
| `agent.load` | ⛔ 永久超时，WebSocket 断连 |
| `agent.execute`（长时间执行的任务） | ⛔ 同上 |
| 耗时 >5s 的任何 RPC | ⛔ 桥接器默认 5s timeout，必超时 |
| WebSocket ping/pong | ❌ 线程阻塞时全部丢失 |
| 其他请求在同一连接上 | ❌ 全部排队等待 |

## 根因

RPC server 采用**单线程 per 连接**模型，所有 WebSocket 帧（包括控制帧）在同一线程中顺序处理。当一个 RPC handler 同步阻塞时，整个连接的所有后续处理（包括 ping/pong/close）都被阻塞。

## 建议方向

将长时间 RPC 操作改为**异步执行**，client_handler 线程只负责帧的收发和解复用：

### 方案 A：线程池 + Future（推荐）

```cpp
// client_handler 只负责帧解码和派发
if (opcode == WS_OPCODE_TEXT) {
    auto future = thread_pool_.enqueue([=]() {
        return handle_request(payload, fd, channel);
    });
    // 存入 pending map，等待 future 完成后再发送响应
    pending_responses_[request_id] = std::move(future);
}
```

### 方案 B：agent.load 改为两步异步

1. `agent.load` 立即返回 `{"status": "loading", "agent_id": "..."}`
2. 后台线程执行 `load_agent` + `start`
3. 通过 `event.subscribe` 或轮询 `agent.list` 获取加载结果

### 方案 C（最小修复）：在 client_handler 中处理超时

在长时间 handler 执行期间，client_handler 线程定期（每 1-2 秒）检查是否有 ping 帧需要处理——但这只能缓解，不能根治。

## 优先级

**P1** — 影响核心功能（通过 UI 加载 agent 永远超时），
