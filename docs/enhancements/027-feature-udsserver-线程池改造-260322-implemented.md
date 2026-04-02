## 2026-03-22 UDSServer 线程池改造

### 1. 背景

#### 1.1 原始问题

当前 UDSServer 采用单线程客户端处理模式，在 `server_loop()` 中阻塞处理每个客户端连接：

```cpp
// 原实现（已改造）
while (running_.load()) {
  int client_fd = accept(server_fd_, nullptr, nullptr);
  handle_client(client_fd);  // 阻塞处理
}
```

**性能瓶颈**：
- Worker 级别 UDS 共享时，多 session 并发访问导致请求排队
- 单 Node 执行时无影响，但多 Node 并发时延迟增加

#### 1.2 实施状态

**已完成**：2026-03-22 提交 `8f1cd1e`

```bash
git log --oneline 8f1cd1e -1
8f1cd1e feat: refactor UDSServer to thread pool model for concurrent client handling
```

### 2. 当前架构

#### 2.1 UDS 生命周期

UDS 生命周期跟随 WorkflowExecutionState，不再是 Worker 级别共享：

```
┌─────────────────────────────────────────────────────────────┐
│                    MMAgentRuntime                            │
├─────────────────────────────────────────────────────────────┤
│  execute_workflow()                                          │
│  ├── 创建 WorkflowExecutionState                             │
│  ├── 创建 UDS Server (socket: /tmp/agentflow_{exec_id}.sock)│
│  └── 注入 uds_socket_path 到 state                           │
├─────────────────────────────────────────────────────────────┤
│  process_node()                                              │
│  └── node->set_databus_uds_path(state.uds_socket_path)      │
├─────────────────────────────────────────────────────────────┤
│  Worker::execute()                                           │
│  ├── 读取 node.databus_uds_path                              │
│  ├── 注入 AF_DATABUS_UDS_FILE 环境变量                        │
│  └── 执行外部进程 (Python/Shell/Go)                           │
├─────────────────────────────────────────────────────────────┤
│  on_workflow_complete()                                      │
│  └── uds_server->stop() → 清理 socket 文件                   │
└─────────────────────────────────────────────────────────────┘
```

#### 2.2 线程池模型

```
┌─────────────────────────────────────────────────────────────┐
│                        UDSServer                             │
├─────────────────────────────────────────────────────────────┤
│  accept_thread_              task_queue_                     │
│       │                         │                           │
│       ▼                         ▼                           │
│  ┌─────────┐              ┌──────────────┐                  │
│  │ accept  │──push───────►│ TaskQueue    │                  │
│  │ client  │              │ (max: 100)   │                  │
│  └─────────┘              └──────┬───────┘                  │
│                                  │ pop                      │
│           ┌──────────────────────┼──────────────────────┐   │
│           │                      │                      │   │
│           ▼                      ▼                      ▼   │
│    ┌───────────┐          ┌───────────┐          ┌───────────┐
│    │ Worker #1 │          │ Worker #2 │   ...    │ Worker #10│
│    │ handle_   │          │ handle_   │          │ handle_   │
│    │ client()  │          │ client()  │          │ client()  │
│    └───────────┘          └───────────┘          └───────────┘
└─────────────────────────────────────────────────────────────┘
```

**默认配置**：
- `thread_pool_size = 10` 个工作线程
- `max_queue_size = 100` 最大排队连接数

### 3. 客户端连接生命周期

#### 3.1 连接创建时机

```
Python/Shell/Go Node 执行流程：

1. Worker::execute() 准备执行环境
   ├── node->get_databus_uds_path() → /tmp/agentflow_{exec_id}.sock
   └── exec_config.env_vars.push_back("AF_DATABUS_UDS_FILE=" + uds_path)

2. 外部进程启动
   └── fork + exec 执行 Python/Shell/Go 脚本

3. 客户端 SDK 初始化
   ├── Python: DataBusClient.from_env() 读取 AF_DATABUS_UDS_FILE
   ├── Go:     databus.NewClientFromEnv()
   └── Shell:  AF_DATABUS_UDS_FILE 环境变量

4. 建立 UDS 连接
   ├── socket(AF_UNIX, SOCK_STREAM)
   └── connect(socket_path)

5. 服务端 accept
   ├── accept_thread_ 接受连接
   ├── push 到 task_queue_
   └── worker_thread 取出处理
```

#### 3.2 连接释放时机

**服务端释放**：

```cpp
// uds_server.cpp:526-615
void UDSServer::handle_client(int client_fd) {
  while (running_.load()) {
    // 1. 检测客户端断开
    if (pfd.revents & (POLLHUP | POLLERR)) {
      break;  // 客户端关闭连接
    }

    // 2. 接收数据失败
    if (n <= 0) {
      break;  // 连接已关闭或出错
    }
  }

  // 3. 清理资源
  active_clients_.erase(client_fd);
  subscriptions_.erase(client_fd 的订阅);
  close(client_fd);
}
```

**客户端释放**：

```python
# Python SDK (agentflow_databus.py:473-485)
def close(self):
    with self._lock:
        if self._socket:
            self._socket.close()
            self._socket = None
        self._connected = False

def __exit__(self, exc_type, exc_val, exc_tb):
    self.close()  # 支持上下文管理器
    return False
```

**进程终止时**：
- 外部进程退出时，OS 自动关闭所有文件描述符
- 服务端收到 `POLLHUP` 事件，触发清理

#### 3.3 连接生命周期图

```
┌──────────────────────────────────────────────────────────────┐
│                     Workflow Execution                        │
├──────────────────────────────────────────────────────────────┤
│                                                               │
│  UDS Server Start ──────────────────────────── UDS Server Stop│
│       │                                              │         │
│       │  execution_id lifecycle                     │         │
│       │                                              │         │
│       ▼                                              ▼         │
│  ┌─────────┐    ┌─────────┐    ┌─────────┐    ┌─────────┐    │
│  │ Node A  │    │ Node B  │    │ Node C  │    │ Node N  │    │
│  │ Python  │    │ Shell   │    │ Go      │    │ ...     │    │
│  └────┬────┘    └────┬────┘    └────┬────┘    └────┬────┘    │
│       │              │              │              │          │
│       ▼              ▼              ▼              ▼          │
│  ┌─────────┐    ┌─────────┐    ┌─────────┐    ┌─────────┐    │
│  │Connect  │    │Connect  │    │Connect  │    │Connect  │    │
│  │Request  │    │Request  │    │Request  │    │Request  │    │
│  │Response │    │Response │    │Response │    │Response │    │
│  │Close    │    │Close    │    │Close    │    │Close    │    │
│  └─────────┘    └─────────┘    └─────────┘    └─────────┘    │
│                                                               │
│  每个节点独立连接，进程结束后自动断开                           │
└──────────────────────────────────────────────────────────────┘
```

### 4. 配置优化分析

#### 4.1 当前配置评估

| 配置项 | 当前值 | 评估 |
|--------|--------|------|
| `thread_pool_size` | 10 | 偏高 |
| `max_queue_size` | 100 | 合理 |

**分析**：

1. **UDS 不再 Worker 级别共享**：
   - 每个 execution 有独立 UDS server
   - 单 execution 内 node 数量通常 3-10 个

2. **单 execution 并发度**：
   - 取决于 workflow 结构（fork/parallel）
   - 典型场景：1-5 个 node 并行执行

3. **线程利用率**：
   - 10 线程处理 1-5 个并发请求
   - 大部分线程处于空闲状态

#### 4.2 推荐配置

**方案 A：保守配置（推荐）**

```cpp
UDSServer::Config config;
config.thread_pool_size = 3;   // 匹配典型并行度
config.max_queue_size = 20;    // 适中的队列容量
```

**方案 B：动态配置**

```cpp
// 根据硬件和场景调整
size_t pool_size = std::min(
    std::max(2UL, std::thread::hardware_concurrency()),
    5UL  // 上限 5
);
```

**方案 C：保持现状**

如果：
- 单 execution 内 node 并发度可能较高（>10）
- 有大量使用 fork/parallel 的 workflow
- 线程资源充足

则保持 `thread_pool_size = 10`。

#### 4.3 配置对比

| 场景 | 线程数 | 队列容量 | 预期效果 |
|------|--------|---------|---------|
| 简单 workflow (串行) | 1-2 | 10 | 足够 |
| 中等复杂度 (少量并行) | 3-4 | 20 | 足够 |
| 复杂 workflow (大量并行) | 5-10 | 50-100 | 当前配置 |

### 5. 线程安全分析

| 资源 | 访问方式 | 保护机制 |
|------|----------|----------|
| TaskQueue | 多线程读写 | mutex + condition_variable |
| active_clients_ | 多线程读写 | clients_mutex_ |
| subscriptions_ | 多线程读写 | subscription_mutex_ |
| MMDataBus | 多线程读写 | ZeroCopyDataBus::shared_mutex |
| running_ | 多线程读写 | std::atomic<bool> |

**关键点**：
1. `handle_client()` 无状态：每个客户端处理独立
2. DataBus 操作线程安全：ZeroCopyDataBus 使用 `shared_mutex`
3. 订阅回调安全：持有 `subscription_mutex_` 时执行

### 6. 边界场景处理

#### 6.1 队列满

```cpp
// uds_server.cpp:506-510
if (!task_queue_.push(client_fd)) {
  send_error_and_close(client_fd, -32000, "Server busy: task queue full");
  Logger::instance().warn("[UDS_SERVER] Task queue full, rejected connection");
}
```

客户端收到 JSON-RPC 错误响应：
```json
{"jsonrpc":"2.0","error":{"code":-32000,"message":"Server busy: task queue full"},"id":-1}
```

#### 6.2 客户端超时

```python
# Python SDK
client = DataBusClient.from_env(timeout_seconds=30.0)
# 30 秒内无响应抛出 DataBusError
```

#### 6.3 服务端关闭

```cpp
// 优雅关闭流程
void UDSServer::stop() {
  running_ = false;           // 1. 停止接受新连接
  task_queue_.stop();         // 2. 唤醒等待的 worker
  accept_thread_.join();      // 3. 等待 accept 线程
  for (auto& t : worker_threads_) t.join();  // 4. 等待 worker
  // 清理资源...
}
```

### 7. 相关代码

| 文件 | 关键代码 |
|------|---------|
| `include/internal/runtime/databus/uds_server.h` | Config 结构体，线程池声明 |
| `src/runtime/databus/uds_server.cpp:449-477` | 线程池启动和 worker 主循环 |
| `src/runtime/databus/uds_server.cpp:479-516` | accept 主循环 |
| `src/runtime/databus/uds_server.cpp:526-615` | handle_client 连接处理 |
| `src/runtime/runtime.cpp:624-644` | UDS 创建 |
| `src/runtime/runtime.cpp:894` | UDS path 注入到 node |
| `src/runtime/worker/worker.cpp:77-117` | 环境变量注入 |

### 8. 实施状态

**状态**：✅ 已实施

**提交记录**：
- `8f1cd1e` (2026-03-22) feat: refactor UDSServer to thread pool model

**测试验证**：
- 单元测试：多客户端并发连接
- 集成测试：SystemAgentMock 多 Node 并发执行

**后续优化**：
- 考虑根据实际使用情况调整默认线程池大小
- 添加配置选项允许运行时调整