## 2026-03-22 UDSServer 线程池改造

### 背景

当前 UDSServer 采用单线程客户端处理模式，在 `server_loop()` 中阻塞处理每个客户端连接：

```cpp
// src/runtime/databus/uds_server.cpp:399-417
while (running_.load()) {
  int client_fd = accept(server_fd_, nullptr, nullptr);

  // 问题：阻塞处理单个客户端，其他客户端必须等待
  handle_client(client_fd);
}
```

**性能瓶颈**：

| 场景 | 当前行为 | 影响 |
|------|----------|------|
| 单 Node 执行 | 阻塞处理 | 无影响 |
| 多 Node 并发执行 | 串行处理 | 客户端请求排队，延迟增加 |
| Worker 级别 UDS 共享 | 多 session 并发访问 | 严重性能瓶颈 |

**目标**：改造为线程池模式，支持多客户端并发处理。

### 设计方案

#### 1. 架构概览

```
┌─────────────────────────────────────────────────────────────┐
│                        UDSServer                             │
├─────────────────────────────────────────────────────────────┤
│  server_loop()              task_queue_                     │
│       │                         │                           │
│       ▼                         ▼                           │
│  ┌─────────┐              ┌──────────────┐                  │
│  │ accept  │──push───────►│ TaskQueue    │                  │
│  │ client  │              │ ┌──────────┐ │                  │
│  └─────────┘              │ │ ClientFd │ │                  │
│                           │ ├──────────┤ │                  │
│                           │ │ ClientFd │ │                  │
│                           │ └──────────┘ │                  │
│                           └──────┬───────┘                  │
│                                  │ pop                      │
│           ┌──────────────────────┼──────────────────────┐   │
│           │                      │                      │   │
│           ▼                      ▼                      ▼   │
│    ┌───────────┐          ┌───────────┐          ┌───────────┐
│    │ Worker #1 │          │ Worker #2 │          │ Worker #N │
│    │ handle_   │          │ handle_   │          │ handle_   │
│    │ client()  │          │ client()  │          │ client()  │
│    └───────────┘          └───────────┘          └───────────┘
│                                                                  │
└─────────────────────────────────────────────────────────────┘
```

#### 2. 核心数据结构

```cpp
// include/internal/runtime/databus/uds_server.h

class UDSServer {
public:
  // 配置参数
  struct Config {
    size_t thread_pool_size = 10;      // 工作线程数量
    size_t max_queue_size = 100;       // 最大排队客户端数
    int client_timeout_ms = 30000;     // 客户端超时（毫秒）
  };

  explicit UDSServer(MMDataBus* databus, const std::string& socket_path,
                     const Config& config = Config{});

private:
  // 任务队列（线程安全）
  struct TaskQueue {
    std::queue<int> queue;                    // client_fd 队列
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> stopped{false};
    size_t max_size;

    bool push(int client_fd);
    bool pop(int& out_client_fd, int timeout_ms);
    void stop();
    size_t size() const;
  };

  // 线程池
  std::vector<std::thread> worker_threads_;
  TaskQueue task_queue_;

  // 配置
  Config config_;
};
```

#### 3. 任务队列实现

```cpp
// src/runtime/databus/uds_server.cpp

bool UDSServer::TaskQueue::push(int client_fd) {
  std::unique_lock<std::mutex> lock(mutex);

  if (queue.size() >= max_size) {
    return false;  // 队列已满，拒绝连接
  }

  queue.push(client_fd);
  cv.notify_one();
  return true;
}

bool UDSServer::TaskQueue::pop(int& out_client_fd, int timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex);

  if (!cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                   [this] { return !queue.empty() || stopped.load(); })) {
    return false;  // 超时
  }

  if (stopped.load() && queue.empty()) {
    return false;  // 队列已停止
  }

  out_client_fd = queue.front();
  queue.pop();
  return true;
}

void UDSServer::TaskQueue::stop() {
  stopped.store(true);
  cv.notify_all();
}
```

#### 4. 服务端主循环改造

```cpp
void UDSServer::server_loop() {
  while (running_.load()) {
    // Use poll to wait for connections with timeout
    struct pollfd pfd;
    pfd.fd = server_fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret = poll(&pfd, 1, 100);  // 100ms timeout
    if (ret < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (ret == 0) continue;  // timeout, check running_

    if (!(pfd.revents & POLLIN)) continue;

    // Accept new connection
    int client_fd = accept(server_fd_, nullptr, nullptr);
    if (client_fd < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        Logger::instance().error_f("[UDS_SERVER] Accept failed: %s", strerror(errno));
      }
      continue;
    }

    // 推送到任务队列（非阻塞）
    if (!task_queue_.push(client_fd)) {
      Logger::instance().warn("[UDS_SERVER] Task queue full, rejecting connection");
      close(client_fd);
      continue;
    }

    Logger::instance().debug_f("[UDS_SERVER] Client queued: fd=%d, queue_size=%zu",
                               client_fd, task_queue_.size());
  }
}
```

#### 5. 工作线程实现

```cpp
void UDSServer::start_worker_threads() {
  for (size_t i = 0; i < config_.thread_pool_size; ++i) {
    worker_threads_.emplace_back([this, i]() {
      Logger::instance().debug_f("[UDS_SERVER] Worker thread %zu started", i);

      while (running_.load()) {
        int client_fd;
        if (task_queue_.pop(client_fd, 100)) {  // 100ms timeout
          // 记录客户端连接
          {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            active_clients_[client_fd] = true;
          }

          Logger::instance().debug_f("[UDS_SERVER] Worker %zu handling client fd=%d", i, client_fd);

          // 处理客户端请求
          handle_client(client_fd);

          // 清理客户端
          {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            active_clients_.erase(client_fd);
          }
          close(client_fd);

          Logger::instance().debug_f("[UDS_SERVER] Worker %zu finished client fd=%d", i, client_fd);
        }
      }

      Logger::instance().debug_f("[UDS_SERVER] Worker thread %zu exiting", i);
    });
  }
}
```

#### 6. 优雅关闭机制

```cpp
void UDSServer::stop() {
  if (!running_.exchange(false)) {
    return;  // 已经停止
  }

  Logger::instance().info("[UDS_SERVER] Stopping...");

  // 1. 停止任务队列
  task_queue_.stop();

  // 2. 关闭 server socket（唤醒 accept）
  if (server_fd_ >= 0) {
    close(server_fd_);
    server_fd_ = -1;
  }

  // 3. 等待 server_loop 线程结束
  if (server_thread_.joinable()) {
    server_thread_.join();
  }

  // 4. 等待所有工作线程结束
  for (auto& thread : worker_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  worker_threads_.clear();

  // 5. 关闭所有活跃客户端连接
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto& [fd, _] : active_clients_) {
      close(fd);
    }
    active_clients_.clear();
  }

  // 6. 清理订阅
  {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    subscriptions_.clear();
    key_to_subscriptions_.clear();
  }

  // 7. 清理 socket 文件
  cleanup_socket();

  Logger::instance().info("[UDS_SERVER] Stopped");
}
```

#### 7. 启动流程

```cpp
bool UDSServer::start() {
  if (running_.load()) {
    return true;
  }

  if (!databus_) {
    Logger::instance().error("[UDS_SERVER] Cannot start: databus is null");
    return false;
  }

  // ... socket 创建和绑定逻辑不变 ...

  running_ = true;

  // 启动工作线程池（先于 server_loop）
  start_worker_threads();

  // 启动 accept 线程
  server_thread_ = std::thread(&UDSServer::server_loop, this);

  Logger::instance().info_f("[UDS_SERVER] Started on %s with %zu worker threads (default: 10)",
                            socket_path_.c_str(), config_.thread_pool_size);
  return true;
}
```

### 线程安全分析

| 资源 | 访问方式 | 保护机制 |
|------|----------|----------|
| TaskQueue | 多线程读写 | mutex + condition_variable |
| active_clients_ | 多线程读写 | clients_mutex_ |
| subscriptions_ | 多线程读写 | subscription_mutex_ |
| MMDataBus | 多线程读写 | ZeroCopyDataBus::shared_mutex |
| running_ | 多线程读写 | std::atomic<bool> |

**关键点**：

1. **handle_client() 无状态**：每个客户端处理独立，不共享可变状态
2. **DataBus 操作线程安全**：ZeroCopyDataBus 使用 `shared_mutex` 保护
3. **订阅回调安全**：回调在持有 `subscription_mutex_` 时执行，避免并发修改

### 配置建议

```cpp
// 默认配置
UDSServer::Config config;
config.thread_pool_size = 10;    // 默认 10 个工作线程
config.max_queue_size = 100;     // 防止连接堆积
config.client_timeout_ms = 30000; // 30 秒超时

// 高并发场景
config.thread_pool_size = std::thread::hardware_concurrency() * 2;
config.max_queue_size = 500;
```

### 边界场景处理

#### 场景 1：队列满，连接被拒绝

当前行为：直接关闭连接，客户端无法区分失败原因。

**改进方案**：发送错误响应后再关闭

```cpp
if (!task_queue_.push(client_fd)) {
  // 发送 JSON-RPC 错误响应
  std::string error_response = build_error_response(
      -32000, "Server busy: task queue full", -1);
  error_response += "\n";
  send(client_fd, error_response.c_str(), error_response.size(), MSG_NOSIGNAL);

  // 短暂延迟确保数据发出
  usleep(1000);
  close(client_fd);

  Logger::instance().warn("[UDS_SERVER] Task queue full, sent error response");
  continue;
}
```

客户端收到明确错误：
```json
{"jsonrpc":"2.0","error":{"code":-32000,"message":"Server busy: task queue full"},"id":-1}
```

#### 场景 2：线程池忙但队列未满

**问题**：客户端请求入队成功，但所有工作线程繁忙，客户端 `recv()` 无限期阻塞。

**改进方案 1：客户端添加 Socket 超时**

```python
# src/runtime/databus/clients/python/agentflow_databus.py

class DataBusClient:
    def __init__(self, socket_path: str, session_id: Optional[str] = None,
                 timeout_seconds: float = 30.0):
        # ...
        self._timeout_seconds = timeout_seconds

    def _call(self, method: str, params: Dict[str, Any]) -> Dict[str, Any]:
        with self._lock:
            self._request_id += 1
            request = {"jsonrpc": "2.0", "method": method, "params": params, "id": self._request_id}

            try:
                # 设置 socket 超时
                self._socket.settimeout(self._timeout_seconds)

                # 发送请求
                message = json.dumps(request) + "\n"
                self._socket.sendall(message.encode('utf-8'))

                # 接收响应
                response_data = b""
                while True:
                    chunk = self._socket.recv(4096)
                    if not chunk:
                        raise DataBusError("Connection closed")
                    response_data += chunk
                    if b"\n" in response_data:
                        break

                # ... 解析响应 ...

            except socket.timeout:
                raise DataBusError(f"Request timeout after {self._timeout_seconds}s")
```

**改进方案 2：服务端队列等待超时（可选）**

```cpp
// TaskQueue 记录入队时间
struct QueuedClient {
  int client_fd;
  std::chrono::steady_clock::time_point enqueue_time;
};

// 工作线程处理前检查等待时长
void worker_thread_func() {
  QueuedClient client;
  if (task_queue_.pop(client, 100)) {
    auto now = std::chrono::steady_clock::now();
    auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - client.enqueue_time).count();

    if (wait_ms > config_.max_queue_wait_ms) {
      // 等待超时，发送错误响应
      std::string error = build_error_response(-32001, "Queue wait timeout", -1);
      error += "\n";
      send(client.client_fd, error.c_str(), error.size(), MSG_NOSIGNAL);
      close(client.client_fd);
      return;
    }

    handle_client(client.client_fd);
  }
}
```

#### 场景对比

| 场景 | 当前行为 | 改进后行为 |
|------|----------|-----------|
| 队列满 | 直接关闭，客户端收到 Connection closed | 发送错误响应，客户端收到 "Server busy" |
| 线程忙、队列未满 | 客户端无限阻塞 | 客户端 30 秒超时，抛出明确异常 |
| 服务端无响应 | 客户端永久阻塞 | 客户端超时退出 |

### 客户端重试机制（可选）

```python
def _call_with_retry(self, method: str, params: Dict,
                     max_retries: int = 3) -> Dict:
    last_error = None
    for attempt in range(max_retries):
        try:
            return self._call(method, params)
        except JSONRPCError as e:
            if e.code == -32000:  # Server busy
                last_error = e
                time.sleep(0.1 * (attempt + 1))  # 指数退避
                continue
            raise
        except DataBusError as e:
            if "timeout" in str(e).lower():
                last_error = e
                time.sleep(0.1 * (attempt + 1))
                continue
            raise
    raise last_error
```

### 客户端兼容性

改造对客户端完全透明，无需修改：

- Python SDK：无需修改
- Go SDK：无需修改
- 环境变量注入：无需修改

### 文件修改清单

| 文件 | 修改内容 |
|------|---------|
| `include/internal/runtime/databus/uds_server.h` | 添加 Config、TaskQueue 结构体，线程池成员变量 |
| `src/runtime/databus/uds_server.cpp` | 实现线程池启动、任务队列、多线程客户端处理、优雅关闭 |

### 测试方案

1. **单元测试**：
   - TaskQueue 并发 push/pop
   - 线程池启动和关闭
   - 多客户端并发连接

2. **集成测试**：
   - SystemAgentMock 多 Node 并发执行
   - multi_language_databus 多语言并发访问

3. **性能测试**：
   - 单线程 vs 线程池吞吐量对比
   - 不同线程数配置的延迟分布

### 实施状态

**设计完成**：✅

**代码实现**：待实施

**测试验证**：待实施

