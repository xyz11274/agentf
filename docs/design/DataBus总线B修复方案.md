# DataBus 总线 B 修复方案

> **关联文档**: [DataBus全面设计文档.md](DataBus全面设计文档.md) §11.1 / §11.2
> **目标**: 修复总线 B 的两个问题——容量不可配、数据永不清理
> **影响范围**: `dataref_bridge.cpp/h`、`daemon/config.h/cpp`、`agent.cpp`、`builtin_dispatcher.cpp`、`databus.h`
> **设计约束**: 不改变 DataRef 的 `databus://global/{hash}` 地址格式，不改变 `resolve_ref` 的行为，不修改 `store_with_ref` 的双份存储设计（框架级，保留）。

---

## 0. 上下文

### 0.1 AgentFlow 实例与部署模式

AgentFlow 有两种运行方式：

**方式一：独立 daemon（agentflowd）**

```bash
./build/agentflowd --listen 0.0.0.0:17891 --package /path/to/outfit-assistant
```

```
┌─ agentflowd 进程 (daemon 实例) ────────────────────────┐
│                                                        │
│  MMAgentRuntime (1 个，daemon 内全局唯一)               │
│  ├── MMAgent "outfit-assistant"                        │
│  │   ├── 总线 A (per-agent, 容量由 agent.json 配置)     │
│  │   └── sessions: {user1:client1, user2:client2, ...} │
│  │                                                      │
│  └── dataref_bridge 总线 B (全局 static, 所有 agent 共享)│
│                                                        │
│  UDSServer   ← Python 节点通过 socket 访问总线 A         │
│  RPCServer   ← 外部通过 JSON-RPC 发请求                  │
└────────────────────────────────────────────────────────┘
```

**方式二：SDK 嵌入**

```cpp
auto runtime = std::make_shared<MMAgentRuntime>(config);
runtime->register_agent(my_agent);
runtime->start();
```

SDK 模式下 `dataref_bridge` 仍然是全局 static——首次 `store_payload` 时自动初始化，**用户无法配置容量和 Redis 地址**。

### 0.2 多实例协作

多个 daemon 实例可通过 Redis 共享 DataRef 存储。这是 `dataref_bridge` 连接 Redis 的设计意图，也是 AgentFlow 支持跨节点多 agent 协作愿景的基础设施。

### 0.3 DataRef 的设计意图

DataRef 并非瞬态数据。从 Owner 索引（10 字段）、Redis 跨节点共享、`find_refs_by_owner` 查询接口等设计看，其意图是**多 agent 共享的可追溯存储**。

**三个使用层次**：

| 层次 | 场景 | DataRef 生命周期 | 清理策略 |
|------|------|-----------------|---------|
| **1. 消息信封** | 单 agent 内 task 间传递（outfit-assistant 现状） | enqueue → dequeue → 对话完成后可丢弃 | 对话级清理 |
| **2. 多 agent 共享** | 同一 daemon 内 agent A 的结果被 agent B 查询 | session 级保留 | session 级清理 |
| **3. 跨 daemon 分布式** | 多个 daemon 通过 Redis 共享数据 | 需 TTL 或引用计数 | 不能单方面删 |

> **本方案适用层次 1**。outfit-assistant 当前只在单 agent 内传递 DataRef。层次 2/3 启用时需重新评估清理策略。

### 0.4 Session 概念澄清

> **关键澄清**：AgentFlow 的 `MMAgentSession` 不是业界标准的 "用户会话"，而是**池化工作槽**。详见 [DataBus全面设计文档.md](DataBus全面设计文档.md) §1.4。

本方案的清理粒度是**对话级**（dialogue），不是 session 级：

```
用户视角:
  Session (对话会话)
    ├── Dialogue 1 (第一次提问 → 最终回复)  ← 对话完成时清理 DataRef
    ├── Dialogue 2 (第二次提问 → 最终回复)  ← 对话完成时清理 DataRef
    └── Dialogue 3 ...

下一次提问的历史对话由 Bridge 层（SessionProvider）传入，
不依赖 DataRef 在总线 B 中保留上一轮的数据。
```

清理时机：与现有的 `BuiltinDispatcher::cleanup_dialogue_data` 一致——每次对话完成（coordinator 发出最终回复，或出错/取消）时触发。

---

## 1. 修复点总览

本方案包含两个**并列修复点**（全部必做）：

| # | 修复点 | 问题 | 影响 |
|---|--------|------|------|
| **1** | 容量参数配置化 | 硬编码 222MB（用户手动改的临时值），SDK 模式完全不可配 | `config.h/cpp`、`dataref_bridge.h/cpp`、`daemon.cpp` |
| **2** | 对话级 DataRef 清理 | 总线 B 数据只增不减，永不清理 | `dataref_bridge.h/cpp`、`builtin_dispatcher.cpp`、`databus.h` |

### 已知问题（不在本方案修复范围）

| 问题 | 说明 | 决策 |
|------|------|------|
| **双份存储浪费** | `store_with_ref` 中 `put("data_ref:{hash}", value)` + `put(key, value)` 存两份，第二份未被 outfit-assistant 使用 | **保留框架设计**。这是 `MMDataBus::store_with_ref` 的接口语义，其他框架使用者可能依赖 `key_to_ref_[key]` 查询或 logical key 存储。在 DataBus 全面设计文档中标注为已知问题（§11.2），不在本方案中修改框架代码。 |

---

## 2. 修复点 1：容量参数配置化

### 2.1 配置层级

```
优先级: CLI > ENV > JSON > 默认值

ENV:
  AF_DATAREF_CAPACITY_MB=512
  AF_DATAREF_REDIS_URL=redis://...

JSON (configs/dev/agentflowd.json):
  "dataref_bridge": {
    "capacity_mb": 512,
    "redis_url": "redis://127.0.0.1:6379"
  }

默认值:
  capacity_mb = 512
  redis_url = "redis://127.0.0.1:6379"
```

### 2.2 DaemonConfig 扩展

```cpp
// include/internal/daemon/config.h

struct DaemonConfig {
    // ... 现有字段 ...

    struct DataRefBridgeConfig {
        int         capacity_mb = 512;
        std::string redis_url = "redis://127.0.0.1:6379";
    };
    DataRefBridgeConfig dataref_bridge_config;
};
```

### 2.3 三层加载（CLI / ENV / JSON）

```cpp
// src/daemon/config.cpp — parse_cli_args() 新增
} else if (arg == "--dataref-capacity-mb") {
    config.dataref_bridge_config.capacity_mb = std::stoi(take_value(i));
} else if (arg == "--dataref-redis-url") {
    config.dataref_bridge_config.redis_url = take_value(i);
}

// apply_env_overrides() 新增
const char* env_cap = std::getenv("AF_DATAREF_CAPACITY_MB");
if (env_cap) {
    int cap = std::atoi(env_cap);
    if (cap > 0) config.dataref_bridge_config.capacity_mb = cap;
}
env_if("AF_DATAREF_REDIS_URL", config.dataref_bridge_config.redis_url);

// load_config_file() 新增
auto dref = dsl::JsonHelper::get_object(root, "dataref_bridge");
if (dref) {
    int64_t cap = dsl::JsonHelper::get_int(dref, "capacity_mb", 0);
    if (cap > 0) config.dataref_bridge_config.capacity_mb = static_cast<int>(cap);
    std::string url = dsl::JsonHelper::get_string(dref, "redis_url");
    if (is_set(url)) config.dataref_bridge_config.redis_url = url;
}
```

### 2.4 dataref_bridge 初始化重构

```cpp
// include/internal/agent/dataref_bridge.h — 新增接口

namespace dataref_bridge {

/// 初始化总线 B（daemon 启动时调用，首次 store_payload 前必须完成）
void init(int capacity_mb, const std::string& redis_url);

// ... 现有接口不变 ...
std::string store_payload(...);
bool resolve_payload(...);
std::vector<std::string> find_refs_by_owner(...);

}  // namespace dataref_bridge
```

```cpp
// src/agent/dataref_bridge.cpp — 重构初始化

namespace {

struct DataRefBridgeState {
    std::once_flag init_once;
    std::unique_ptr<runtime::MMDataBus> bus;
    std::string redis_url;
    std::atomic<bool> initialized{false};
};

DataRefBridgeState& state() {
    static DataRefBridgeState s;
    return s;
}

void init_impl(int capacity_mb, const std::string& redis_url) {
    auto& s = state();
    std::call_once(s.init_once, [&]() {
        size_t capacity = static_cast<size_t>(capacity_mb) * 1024 * 1024;
        s.bus = std::make_unique<runtime::MMDataBus>(capacity);
        s.redis_url = redis_url;
        s.initialized.store(true);
        if (!redis_url.empty()) {
            s.bus->connect_remote(redis_url);
        }
        Logger::instance().info_f(
            "[dataref_bridge] Initialized: capacity=%d MB, redis=%s",
            capacity_mb, redis_url.empty() ? "disabled" : redis_url.c_str());
    });
}

runtime::MMDataBus& control_databus() {
    auto& s = state();
    if (!s.initialized.load()) {
        // 兼容 SDK 模式：未显式 init 时用安全默认值
        init_impl(512, "redis://127.0.0.1:6379");
    }
    return *s.bus;
}

}  // namespace

void dataref_bridge::init(int capacity_mb, const std::string& redis_url) {
    init_impl(capacity_mb, redis_url);
}
```

### 2.5 daemon 启动时调用 init

```cpp
// src/daemon/daemon.cpp — start() 中，注册 agent 之前

dataref_bridge::init(
    config_.dataref_bridge_config.capacity_mb,
    config_.dataref_bridge_config.redis_url);

// 然后注册 agents
```

---

## 3. 修复点 2：对话级 DataRef 清理

### 3.1 设计思路

**不改变 DataRef 地址格式**。DataRef 地址仍然是 `databus://global/{hash}`，`resolve_ref` 行为不变，`store_with_ref` 的双份存储行为不变。

清理策略：
- 在 `store_payload` 时，metadata 中的 `dialogue_id` 已存在（通过 `message.metadata["dialogue_id"]` 流转）
- 将 `dialogue_id` 加入 OwnerContext，使 `store_with_ref` 在 Redis 中建立 `owner:dialogue_id:{did}` 索引
- 在已有的 `BuiltinDispatcher::cleanup_dialogue_data` 收敛点追加总线 B 清理
- 通过 `find_refs_by_owner("dialogue_id", dialogue_id)` 查找并删除该对话的所有 DataRef

```
对话生命周期:

  用户提问 → bridge → daemon → dispatcher → coordinator → skills → 最终回复
                                                                      │
                                                                      ▼
                                                    BuiltinDispatcher::cleanup_dialogue_data
                                                      ├── 清理总线A: /session/{sid}/sys/{did}/  (已有)
                                                      ├── 清理总线A: /session/{sid}/stream/{did}/ (已有)
                                                      ├── 清理总线A: /session/{sid}/ctx/{did}/   (已有)
                                                      └── 清理总线B: find_refs_by_owner("dialogue_id", did)  ← 新增
```

**为什么是对话级而非 session 级**：
- outfit-assistant 的 DataRef 用法是层次 1（消息信封），对话完成后数据不再需要
- 下一次提问的历史对话由 Bridge 层（SessionProvider）传入，不依赖总线 B 保留上一轮数据
- 对话级清理是同步的，不存在 session 老化的 seq 竞态问题
- 与总线 A 的 `cleanup_dialogue_data` 时机完全一致

### 3.2 OwnerContext 扩展

```cpp
// include/internal/runtime/databus/databus.h — OwnerContext 新增字段

struct OwnerContext {
    std::string request_id;
    std::string user_id;
    std::string client_id;
    std::string agent_id;
    std::string session_id;
    std::string dialogue_id;    // ← 新增
    std::string task_id;
    std::string workflow_id;
    std::string node_id;
    std::string master_id;
    std::string worker_id;
};
```

```cpp
// src/agent/dataref_bridge.cpp — build_owner 新增拷贝

runtime::MMDataBus::OwnerContext build_owner(const std::map<std::string, std::string>& metadata) {
    runtime::MMDataBus::OwnerContext owner;
    const auto copy = [&metadata](const char* key, std::string& target) {
        auto it = metadata.find(key);
        if (it != metadata.end()) {
            target = it->second;
        }
    };
    copy("request_id", owner.request_id);
    copy("user_id", owner.user_id);
    copy("client_id", owner.client_id);
    copy("agent_id", owner.agent_id);
    copy("session_id", owner.session_id);
    copy("dialogue_id", owner.dialogue_id);    // ← 新增
    copy("task_id", owner.task_id);
    copy("workflow_id", owner.workflow_id);
    copy("node_id", owner.node_id);
    copy("master_id", owner.master_id);
    copy("worker_id", owner.worker_id);
    return owner;
}
```

`store_with_ref` 中的 owner 索引循环已有 `add_owner_index` 调用模式，新增一行：

```cpp
// src/runtime/databus/databus.cpp — store_with_ref 中新增

if (remote_bus_->is_connected()) {
    const std::string ref_address = out_ref.address;
    add_owner_index(remote_bus_.get(), "request_id", owner.request_id, ref_address);
    add_owner_index(remote_bus_.get(), "user_id", owner.user_id, ref_address);
    add_owner_index(remote_bus_.get(), "client_id", owner.client_id, ref_address);
    add_owner_index(remote_bus_.get(), "agent_id", owner.agent_id, ref_address);
    add_owner_index(remote_bus_.get(), "session_id", owner.session_id, ref_address);
    add_owner_index(remote_bus_.get(), "dialogue_id", owner.dialogue_id, ref_address);  // ← 新增
    add_owner_index(remote_bus_.get(), "task_id", owner.task_id, ref_address);
    add_owner_index(remote_bus_.get(), "workflow_id", owner.workflow_id, ref_address);
    add_owner_index(remote_bus_.get(), "node_id", owner.node_id, ref_address);
    add_owner_index(remote_bus_.get(), "master_id", owner.master_id, ref_address);
    add_owner_index(remote_bus_.get(), "worker_id", owner.worker_id, ref_address);
}
```

### 3.3 cleanup_dialogue 接口

```cpp
// include/internal/agent/dataref_bridge.h — 新增

namespace dataref_bridge {

/// 清理指定对话的所有 DataRef 数据
/// @param dialogue_id 对话 ID（UUID）
/// @return 清理的 DataRef 条目数
int cleanup_dialogue(const std::string& dialogue_id);

}  // namespace dataref_bridge
```

```cpp
// src/agent/dataref_bridge.cpp — 新增

int dataref_bridge::cleanup_dialogue(const std::string& dialogue_id) {
    if (dialogue_id.empty()) return 0;

    auto& bus = control_databus();

    // 依赖 remote redis owner index 做按 dialogue_id 的精准清理
    if (!bus.remote() || !bus.remote()->is_connected()) { ... return -1; }

    auto refs = bus.find_refs_by_owner("dialogue_id", dialogue_id);
    if (refs.empty()) { ... return 0; }

    int cleared = 0;
    const std::string addr_prefix = "databus://global/";
    for (const auto& ref : refs) {
        if (ref.rfind(addr_prefix, 0) != 0) continue;

        std::string data_id = ref.substr(addr_prefix.size());
        std::string data_key = "data_ref:" + data_id;

        // ① 删 data_ref:{hash} 份（payload 本体）
        if (bus.remove(data_key)) ++cleared;

        // ② 通过反向映射删除 logical key 份（框架级修复，见 §3.3.1）
        std::string logical_key_str;
        DataVariant lk_val;
        if (bus.get("data_ref_logical:" + data_id, lk_val)) {
            if (const auto* lk = std::get_if<std::string>(&lk_val)) {
                logical_key_str = *lk;
            }
        }
        if (!logical_key_str.empty()) bus.remove(logical_key_str);
        bus.remove("data_ref_logical:" + data_id);
    }

    bus.clear_owner_index("dialogue_id", dialogue_id);
    return cleared;
}
```

### 3.3.1 框架级修复：反向映射根治漏删

**问题**：`store_with_ref` 双份存储（`data_ref:{hash}` + logical_key），`cleanup_dialogue` 只删了 `data_ref:{hash}` 份，logical key 份永久残留。

**修复**：在 `MMDataBus::store_with_ref`（`src/runtime/databus/databus.cpp`）中新增反向映射：

```cpp
// 存完后追加一条映射
put("data_ref_logical:" + data_id, key);
```

`cleanup_dialogue` 通过该映射找到 logical key 后一并删除。

**特性**：
- 框架级修复，所有 `store_with_ref` 使用者受益
- 反向映射与 payload 同生命周期，cleanup 时自动清除，无新残留
- `MMDataBus::put/remove` 同时作用本地 shared memory 和远程 Redis

### 3.4 挂载到现有对话清理收敛点

`BuiltinDispatcher::cleanup_dialogue_data` 是所有对话完成路径的收敛点（agent.cpp 中 5 处调用：正常完成、错误、取消、中断）。在此追加总线 B 清理：

```cpp
// src/agent/builtin_dispatcher.cpp — cleanup_dialogue_data 末尾追加

void BuiltinDispatcher::cleanup_dialogue_data(MMAgent& agent,
                                                const std::string& session_id,
                                                const std::string& dialogue_id) {
    if (dialogue_id.empty()) {
        return;
    }

    Logger::instance().info_f("[CLEANUP_DIALOGUE] Cleaning up dialogue_id=%s", dialogue_id.c_str());

    // ── 总线 A 清理（现有代码，不变）──
    // ... launched_skills 读取 ...
    // ... sys/dispatch/stream/ctx/llm/tool 前缀清理 ...

    // ── 总线 B 清理（新增）──
    int bus_b_cleared = dataref_bridge::cleanup_dialogue(dialogue_id);
    if (bus_b_cleared > 0) {
        Logger::instance().info_f(
            "[CLEANUP_DIALOGUE] Cleaned %d DataRef entries (Bus-B) for dialogue_id=%s",
            bus_b_cleared, dialogue_id.c_str());
    }

    Logger::instance().info_f("[CLEANUP_DIALOGUE] Cleaned up %zu skills for dialogue_id=%s",
                              launched_skills.size(), dialogue_id.c_str());
}
```

### 3.5 RedisAdapter 扩展

清理需要删除 Redis 中的 owner 索引集合。需在 `RedisAdapter` 和 `MMDataBus` 中新增 `clear_owner_index`：

```cpp
// include/internal/runtime/databus/redis_adapter.h — 新增
class RedisAdapter {
public:
    // ... 现有接口 ...
    bool clear_owner_index(const std::string& owner_key, const std::string& owner_value);
};

// include/internal/runtime/databus/databus.h — 新增透传
class MMDataBus {
public:
    // ... 现有接口 ...
    void clear_owner_index(const std::string& owner_key, const std::string& owner_value);
};
```

```cpp
// src/runtime/databus/redis_adapter.cpp — 实现
bool RedisAdapter::clear_owner_index(const std::string& owner_key,
                                      const std::string& owner_value) {
    if (!connected_) return false;
    std::string index_key = "owner:" + owner_key + ":" + owner_value;
    redisReply* reply = static_cast<redisReply*>(
        redisCommand(context_, "DEL %s", index_key.c_str()));
    if (!reply) return false;
    bool ok = (reply->type == REDIS_REPLY_INTEGER);
    freeReplyObject(reply);
    return ok;
}
```

### 3.6 无 Redis 时的退化行为

当 Redis 未连接时，`find_refs_by_owner` 返回空列表（已有行为），`cleanup_dialogue` 跳过清理并记录 debug 日志。

**风险评估**：无 Redis 时无法做对话级清理。但无 Redis 时 DataRef 的跨节点能力本身不可用，退化为纯本地存储。数据会在总线 B 中累积，最终触发 "shared memory full"。容量保护由修复点 1 的可配置化缓解（可设大容量 + 定期重启 daemon）。

---

## 4. 并发安全分析

### 4.1 线程模型

```
Daemon Main Thread (启动)      ──→ init()
Worker Thread 1 (coordinator)  ──┐
Worker Thread 2 (skill-runner)  ──┼──→ 并发 store_payload() / resolve_payload()
Worker Thread 3 (dispatcher)   ──┘
Coordinator Thread             ──→ cleanup_dialogue_data() (对话完成时)
```

### 4.2 并发安全保证

| 操作 | 锁机制 | 安全性 |
|------|--------|--------|
| `store_payload` → `store_with_ref` → `put` | ZeroCopyDataBus 内部 `shared_mutex` 写锁 | ✅ |
| `resolve_payload` → `resolve_ref` → `get` | ZeroCopyDataBus 内部 `shared_mutex` 读锁 | ✅ |
| `cleanup_dialogue` → `find_refs_by_owner` | Redis 只读查询 | ✅ |
| `cleanup_dialogue` → `remove(data_key)` | ZeroCopyDataBus 内部 `shared_mutex` 写锁 | ✅ |
| `cleanup_dialogue` → `clear_owner_index` | Redis DEL 操作 | ✅ |
| `init()` | `std::call_once` | ✅ |

### 4.3 对话串行化保证

同一 session 内的对话是**串行执行**的（`MMAgentSession::acquire_dialogue_lock`）。因此 `cleanup_dialogue_data` 执行时：
- 该 session 不会有另一个对话并发写入 DataRef
- 不存在 cleanup 与 store 的竞态
- 无需额外的 seq 校验（与 session 级清理不同）

### 4.4 残留风险

| 风险 | 严重性 | 说明 | 缓解 |
|------|--------|------|------|
| 无 Redis 时无法清理 | 🟡 中 | owner 索引不存在 | 容量配置化兜底；定期重启 |
| `dialogue_id` 未传入 metadata | 🟢 低 | `store_payload` 时 metadata 缺 dialogue_id | `build_owner` 做了 find+empty 保护；该次 store 不被清理（可接受） |
| 层次 2/3 场景下误删共享数据 | 🟡 中 | 多 agent 共享时对话级清理可能删掉其他 agent 正在用的 DataRef | 当前只适用层次 1；层次 2/3 启用时需改为 session 级或引用计数 |

---

## 5. agentflowd.json 配置示例

```json
{
    "server": {
        "listen": "0.0.0.0:17891",
        "uds_biz_path": "/tmp/agentflowd_biz.sock",
        "uds_ctrl_path": "/tmp/agentflowd_ctrl.sock"
    },
    "dataref_bridge": {
        "capacity_mb": 512,
        "redis_url": "redis://127.0.0.1:6379"
    },
    "agents": [
        { "package_path": "/opt/agentflow/agents/outfit-assistant" }
    ],
    "logging": {
        "level": "info"
    }
}
```

---

## 6. 改动清单

| # | 文件 | 改动 | 修复点 | 优先级 |
|---|------|------|--------|--------|
| 1 | `include/internal/daemon/config.h` | 新增 `DataRefBridgeConfig` 结构体 | 1 | P0 |
| 2 | `src/daemon/config.cpp` | CLI / ENV / JSON 三层加载 | 1 | P0 |
| 3 | `include/internal/agent/dataref_bridge.h` | 新增 `init()` / `cleanup_dialogue()` 接口 | 1+2 | P0 |
| 4 | `src/agent/dataref_bridge.cpp` | init 模式重构 + cleanup_dialogue 实现 + build_owner 新增 dialogue_id | 1+2 | P0 |
| 5 | `src/daemon/daemon.cpp` | 启动时调用 `dataref_bridge::init()` | 1 | P0 |
| 6 | `include/internal/runtime/databus/databus.h` | OwnerContext 新增 `dialogue_id` 字段 + `clear_owner_index()` 透传 | 2 | P0 |
| 7 | `src/runtime/databus/databus.cpp` | `store_with_ref` 中新增 `dialogue_id` owner 索引 | 2 | P0 |
| 8 | `src/agent/builtin_dispatcher.cpp` | `cleanup_dialogue_data` 末尾追加 `dataref_bridge::cleanup_dialogue()` | 2 | P1 |
| 9 | `include/internal/runtime/databus/redis_adapter.h` | 新增 `clear_owner_index()` | 2 | P1 |
| 10 | `src/runtime/databus/redis_adapter.cpp` | 实现 `clear_owner_index()` | 2 | P1 |
| 11 | `src/runtime/databus/databus.cpp` | `store_with_ref` 新增反向映射 `put("data_ref_logical:"+data_id, key)`（§3.3.1） | 2 | P0 |
| 12 | `src/agent/dataref_bridge.cpp` | `cleanup_dialogue` 通过反向映射补删 logical key 份 + 映射自身 | 2 | P0 |
| 13 | `tests/e2e/databus_cleanup/test_databus_cleanup_e2e.cpp` | 新增 `CleanupDialogueLeavesLogicalKeyLeak` 用例锁住漏删缺陷 | 2 | P1 |
| 14 | `docs/design/DataBus总线B修复方案.md` | 新增 §3.3.1 框架级反向映射修复 | — | P0 |

---

## 7. 验证计划

### 7.1 配置化验证（修复点 1）

```bash
# 环境变量方式
AF_DATAREF_CAPACITY_MB=256 ./build/agentflowd --package examples/agent/outfit-assistant

# 验证日志
grep "dataref_bridge.*Initialized" daemon.log
# 期望: [dataref_bridge] Initialized: capacity=256 MB, redis=redis://127.0.0.1:6379
```

### 7.2 对话级清理验证（修复点 2）

```bash
# 发送一次请求
curl -X POST http://localhost:17891/api/chat/outfit-assistant \
    -d '{"message":"今天穿什么"}'

# 对话完成后检查清理日志
grep "cleanup_dialogue" daemon.log
# 期望: [dataref_bridge] cleanup_dialogue: dialogue=xxx, found=N, cleared=N

# 连续发送多次请求，验证每次对话后都清理
grep "CLEANUP_DIALOGUE.*DataRef" daemon.log
# 期望: 每次对话后都出现 Bus-B 清理日志
```

### 7.3 容量稳定性验证

```bash
# 发送大量请求后检查容量未耗尽
for i in $(seq 1 1000); do
    curl -X POST http://localhost:17891/api/chat/outfit-assistant \
        -d '{"message":"测试请求 '$i'"}'
done

grep "Shared memory full" daemon.log
# 期望: 无输出（每次对话后清理，容量不累积）
```

### 7.4 回归测试

```bash
# C++ E2E 单元测试（锁住漏洞 ①-⑨）
ctest --test-dir build -R E2EDataBusCleanup --output-on-failure

# Python 黑盒 E2E 测试（启动 daemon → 发请求 → 验证 Bus-B 清理日志）
# 需 agentflowd 已编译 + pytest + websockets 库
# 无 Redis 时只验证 CLEANUP_DIALOGUE 时机，有 Redis 时验证 found/cleared 计数
cd <project_root>
python3 -m pytest frontend/bridge/tests/test_databus_cleanup_e2e.py -v
```
