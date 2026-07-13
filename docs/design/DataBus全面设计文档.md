# DataBus 全面设计文档

> **版本**: v1.1
> **日期**: 2026-07-09
> **状态**: v1.1 — 补充部署模式与 DataRef 设计意图
> **范围**: AgentFlow C++ 框架 DataBus 子系统的完整架构、数据流、配置、已知问题

---

## 目录

1. [概述](#1-概述)
2. [架构总览](#2-架构总览)
3. [双总线模型](#3-双总线模型)
4. [实例生命周期管理](#4-实例生命周期管理)
5. [数据操作模式](#5-数据操作模式)
6. [DataRef 与控制面/数据面分离](#6-dataref-与控制面数据面分离)
7. [容量配置体系](#7-容量配置体系)
8. [Session 清理机制](#8-session-清理机制)
9. [UDS 多语言访问](#9-uds-多语言访问)
10. [Redis 远程同步](#10-redis-远程同步)
11. [已知问题与风险](#11-已知问题与风险)
12. [关键文件索引](#12-关键文件索引)

---

## 1. 概述

### 1.1 DataBus 是什么

DataBus 是 AgentFlow 框架的**数据面核心**，负责在 Agent 运行期间的各个组件之间共享数据：

- **Task 间通信**：dispatcher → coordinator → skill 之间传递用户输入、LLM 结果、skill 产出
- **节点数据交换**：workflow 节点通过 DataBus 读写中间结果（非文件传递）
- **会话状态持久化**：对话历史、LLM trace、context 数据
- **控制面消息解耦**：消息队列只传 DataRef 地址，实际 payload 存在 DataBus

### 1.2 设计目标

| 目标 | 实现方式 |
|------|---------|
| 零拷贝数据共享 | `ZeroCopyDataBus` + `DataVariant` 指针访问 |
| 本地/远程统一接口 | `MMDataBus` 封装 `LocalDatabus` + `RemoteDatabus` |
| 多语言节点支持 | UDS Server 暴露 JSON 协议，Python/Java/Go 节点通过 socket 访问 |
| 控制面轻量化 | DataRef 模式：消息只传地址，payload 留在 DataBus |

### 1.3 AgentFlow 部署模式与 DataBus 的关系

AgentFlow 有两种运行方式，DataBus 的实例拓扑在这两种模式下不同：

**方式一：独立 daemon（agentflowd）**

```bash
./build/agentflowd --listen 0.0.0.0:17891 --package /path/to/agent-a --package /path/to/agent-b
```

一个 daemon 进程可以 host 多个 agent，共享同一个 `dataref_bridge`（总线 B）：

```
┌─ agentflowd 进程 ───────────────────────────────────────────┐
│  MMAgentRuntime (1 个)                                       │
│  ├── MMAgent "agent-a"  → 总线 A (per-agent, agent.json 配置) │
│  ├── MMAgent "agent-b"  → 总线 A (per-agent)                 │
│  └── dataref_bridge 总线 B (全局 static, 所有 agent 共享)     │
│  UDSServer / RPCServer                                      │
└─────────────────────────────────────────────────────────────┘
```

**方式二：SDK 嵌入**

```cpp
auto runtime = std::make_shared<MMAgentRuntime>(config);
runtime->register_agent(my_agent);
runtime->start();
```

用户应用程序链接 `libagentflow.a`，直接调用 runtime API。`dataref_bridge` 仍然是全局 static，首次 `store_payload` 时自动初始化。

**多实例协作（跨节点）**

多个 daemon 实例可通过 Redis 共享 DataRef 存储，实现跨节点多 agent 协作：

```
机器 1 (agentflowd-1)          机器 2 (agentflowd-2)
  agent "outfit"                 agent "kitchen"
  总线 B (本地)                  总线 B (本地)
       ↘                    ↙
        Redis (共享 DataRef)
```

实例 1 上 agent 存的数据，实例 2 上 agent 可通过 Redis 查到。这是 `dataref_bridge` 连接 Redis 的设计意图，也是 AgentFlow 支持跨节点多 agent 协作愿景的基础设施。

### 1.4 Session 概念错位与对话数据模型

> **⚠️ 关键澄清**：AgentFlow 的 `MMAgentSession` 不是业界标准的 "用户会话"。理解这一错位是正确设计 DataBus 清理机制的前提。

#### 三层概念对照

AgentFlow 体系内有三个概念，它们的对应关系容易混淆：

| 业界标准概念 | AgentFlow 中的对应物 | 代码位置 | 生命周期 |
|-------------|---------------------|---------|---------|
| **用户会话 (Session)** — 一个用户的对话上下文，跨请求持久 | Bridge Session (`SessionProvider`) | `frontend/bridge/providers/` | 跨重启持久化，keyed by `(user_id, client_id)` |
| **对话轮次 (Turn / Message)** — 一次请求 + 一次回复 | `dialogue_id` | `session.h:DialogueEventCallback` | 随请求生成（UUID），完成后清理 |
| **工作槽 (Worker Slot)** — 执行资源，可复用 | `MMAgentSession` | `include/internal/agent/session.h` | 从池中分配→绑定 `(user_id, client_id)`→老化后回收→可被新用户复用 |

**核心错位**：`MMAgentSession` 被命名为 "session"，但它的行为是**池化工作槽**——可以被不同用户复用，不是某个用户的持久会话。业界称为 "session" 的概念在 AgentFlow 中由 Bridge 层的 `SessionProvider` 承担。

#### 对话数据模型完整映射

```
用户视角              Bridge 层 (Python)          AgentFlow C++ 层              DataBus Key
──────────           ──────────────              ──────────────                ──────────
对话会话       →     Bridge Session         →    route_key                     N/A
(chat history)       (user_id + client_id)        (user_id:client_id)
                      ├─ agentflow_session_id     │
                      │   (引用, 可变)             ├─ MMAgentSession
                      │                            │   ├─ session_id (UUID)
                      ├─ dialogues[]               │   ├─ active_seq
                      │   └─ dialogue_id    →     ├── dialogue_id          →   /session/{session_id}/
                      │       (per request)        │   └─ dialogue_lock         (总线A key 前缀)
                      └─ messages[]                │                             ├─ sys/{dialogue_id}/
                           (持久化)                │                             ├─ user/{dialogue_id}/
                                                 │                             ├─ stream/{dialogue_id}/
                                                 │                             └─ ctx/

                           DataRef (总线B)     ←   store_payload(metadata)   →   data_ref:{content_hash}
                           owner.session_id    ←   build_owner(metadata)     →   Redis: owner:session_id:{sid}
                                                                                  (owner 索引, 非总线 key)
```

#### Bridge Session vs MMAgentSession

| 维度 | Bridge Session | MMAgentSession |
|------|---------------|----------------|
| **ID 稳定性** | 稳定（跨 daemon 重启不变） | 瞬态（pool 回收后 session_id UUID 不变但绑定关系变化） |
| **键** | `(user_id, client_id)` | `session_id` (UUID)，绑定 `route_key = user_id:client_id` |
| **持久化** | SQLite / KMM HTTP | 纯内存 |
| **复用** | 不可复用（一个用户一个 session） | 可复用（pool 回收后给新用户用） |
| **内容** | 消息历史、trace、标题 | task 模板、消息队列、dialogue 锁 |

Bridge 层的设计文档明确声明（`session_provider.py:46-48`）：

> Bridge session IDs are stable across agentflow session recycling (I-1: Bridge session does not depend on agentflow session ID).

#### 对 DataBus 清理的影响

DataBus 的 key 以 `MMAgentSession.session_id`（UUID）为前缀：

```
/session/{session_id}/sys/{dialogue_id}/responses/content
          ^^^^^^^^^^^^^^
          MMAgentSession 的 UUID，不是用户会话 ID
```

清理时机绑定在 `MMAgentSession` 的**老化回收**上（`SessionCleanupCallback`），而非用户的"退出登录"。当 session 老化（默认 1 小时无活动），其所有 DataBus 数据被清理。

> **详见**: §8 Session 清理机制；[DataBus总线B修复方案.md](DataBus总线B修复方案.md) §0.4 / §4

---

## 2. 架构总览

### 2.1 类层次

```
MMDataBus (统一入口)
├── LocalDatabus (进程内数据)
│   ├── LocalChannel (消息队列, 有序投递)
│   └── ZeroCopyDataBus (KV 存储, 容量受限)
│       └── SharedMemoryBlock (接口)
│           ├── InProcessSharedMemoryBlock (堆内存实现)
│           ├── PosixSharedMemoryBlock (shm_open + mmap)
│           └── SysVSharedMemoryBlock (shmget + shmat)
└── RemoteDatabus (跨节点数据)
    ├── data_store_ (本地缓存 map)
    ├── RedisAdapter (Redis 后端)
    └── owner_index_ (DataRef 所有者索引)
```

### 2.2 代码包结构（导航图）

```
include/internal/                          ← 接口定义层（头文件）
│
├── runtime/databus/                       【DataBus 接口包】
│   ├── databus.h                          MMDataBus — 统一入口接口
│   ├── local_databus.h                    LocalDatabus / ZeroCopyDataBus / SharedMemoryBlock / SharedMemoryFactory
│   ├── remote_databus.h                   RemoteDatabus — Redis 远程同步接口
│   ├── uds_server.h                       UDSServer — 多语言 DataBus 访问服务
│   ├── node_databus_client.h              节点侧 DataBus 客户端接口
│   ├── node_databus_adapter.h             节点侧 DataBus 适配器接口
│   ├── namespace_client.h                 命名空间客户端接口
│   └── file_databus_bridge.h              文件桥接接口
│
├── runtime/                               【运行时接口包】
│   ├── agent_runtime.h                    MMAgentRuntime — DataBus 创建/管理/查找
│   └── worker.h                           MMWorkerEngine — DataBus 注入到 worker
│
└── agent/                                 【Agent 框架接口包】
    ├── dataref_bridge.h                   DataRef Bridge — 总线 B 接口
    ├── msgque.h                           MMAgentMsgQue — 消息队列（DataRef 使用方）
    ├── agent.h                            MMAgent — session cleanup 注册
    ├── coordinator.h                      Coordinator — DataRef 解析
    ├── context.h                          Context — DataRef 存储
    ├── session_router.h                   SessionRouter — 老化清理触发
    └── package.h                          AgentPackage — data_bus_capacity_mb 解析


src/                                       ← 实现层（源文件）
│
├── runtime/databus/                       【DataBus 核心实现包】
│   ├── databus.cpp                        MMDataBus: put/get/remove/store_with_ref/resolve_ref
│   ├── local_databus.cpp                  ★ InProcessSharedMemoryBlock (容量检查 + "Shared memory full" 错误)
│   │                                        ZeroCopyDataBus (put/get/subscribe/clear)
│   │                                        SharedMemoryFactory (create_best/create_in_process/...)
│   │                                        LocalDatabus (组合 Channel + ZeroCopy)
│   ├── remote_databus.cpp                 RemoteDatabus: Redis publish/fetch/index
│   ├── uds_server.cpp                     UDSServer: JSON 协议解析 + runtime.get_databus() 代理
│   ├── node_databus_client.cpp            节点侧客户端实现
│   ├── namespace_client.cpp               命名空间客户端实现
│   └── file_databus_bridge.cpp            文件桥接实现
│
├── runtime/                               【运行时实现包】
│   └── runtime.cpp                        ★ 3 条 DataBus 创建路径 (register_agent / queue / get_databus lazy)
│
├── agent/                                 【Agent 框架实现包】
│   ├── dataref_bridge.cpp                 ★ 全局静态 MMDataBus(4MB) — 总线 B 根因
│   ├── msgque.cpp                         enqueue: store_payload → content 清空; dequeue: resolve_payload → content 还原
│   ├── agent.cpp                          session cleanup callback → clear_session_data; build_runtime_task → store/resolve
│   ├── coordinator.cpp                    coordinator 解析 message.data_ref
│   ├── context.cpp                        context 存储 payload
│   └── package.cpp                        解析 agent.json → data_bus_capacity_mb
│
└── daemon/                                【Daemon RPC 包】
    └── rpc_handler.cpp                    JSON-RPC: databus.put/get/list/clear 代理


src/runtime/databus/clients/python/        【Python 客户端包】
├── agentflow_databus.py                   Python DataBus 客户端 (UDS socket 通信)
└── examples/basic_usage.py                使用示例


vendor/adapter/cache/                      【Redis 适配包】
└── redis_adapter.cpp                      RedisAdapter: connect/set/get/del/sadd/smembers


tests/                                     【测试包】
├── unit/src/
│   ├── test_databus.cpp                   基础功能测试
│   ├── test_databus_gtest.cpp             GoogleTest 数据总线测试
│   ├── test_databus_concurrent.cpp        并发安全测试
│   └── test_databus_session_isolation.cpp Session 隔离测试
├── e2e/
│   ├── databus_cleanup/                   E2E 清理测试
│   └── blackbox/                          黑盒 UDS 客户端测试
└── ...
```

#### 包间依赖关系

```
                    ┌──────────────────────────────────────────┐
                    │           daemon/rpc_handler              │
                    │         (JSON-RPC 代理 DataBus 操作)       │
                    └──────────────────┬───────────────────────┘
                                       │ runtime.get_databus()
                                       ▼
┌──────────────┐   uses   ┌────────────────────────┐   creates   ┌─────────────────┐
│  agent/      │◄────────│  runtime/               │◄──────────│  agent/          │
│  dataref_    │         │  runtime.cpp            │            │  package.cpp     │
│  bridge.cpp  │         │  (3 条创建路径)          │            │  (解析 capacity)  │
│  (总线 B)    │         └───────────┬────────────┘            └─────────────────┘
└──────┬───────┘                     │
       │ store/resolve               │ new MMDataBus(capacity)
       ▼                             ▼
┌──────────────┐             ┌────────────────────────┐
│  agent/      │  put/get    │  runtime/databus/      │
│  msgque.cpp  │◄───────────►│  databus.cpp           │
│  (消息队列)   │             │  local_databus.cpp     │
└──────────────┘             │  remote_databus.cpp    │
                             │  uds_server.cpp        │
┌──────────────┐             │                        │
│  agent/      │  put/get    │  ┌──────────────────┐  │
│  agent.cpp   │◄───────────►│  │ ZeroCopyDataBus  │  │
│  coordinator │             │  │   └─ InProcess   │  │
│  context     │             │  │      SharedMem   │  │
└──────────────┘             │  │      Block       │  │
                             │  └──────────────────┘  │
┌──────────────┐             └───────────┬────────────┘
│  Python      │   UDS socket             │
│  agentflow_  │◄─────────────────────────┘
│  databus.py  │
└──────────────┘
```

#### 快速查找指南

| 我想找... | 去这个文件 |
|---------|----------|
| "Shared memory full" 错误从哪来 | `src/runtime/databus/local_databus.cpp:534` |
| 总线 B（4MB 硬编码）在哪 | `src/agent/dataref_bridge.cpp:12` |
| agent.json 的 capacity 怎么解析 | `src/agent/package.cpp:324-331` |
| DataBus 实例在哪创建 | `src/runtime/runtime.cpp:332 / 1813 / 2326` |
| session 清理 callback 在哪注册 | `src/agent/agent.cpp:598` |
| 消息队列怎么存/取 payload | `src/agent/msgque.cpp:59-66 / 82-83` |
| Python 节点怎么访问 DataBus | `src/runtime/databus/uds_server.cpp` + `clients/python/agentflow_databus.py` |
| Redis 适配器在哪 | `vendor/adapter/cache/redis_adapter.cpp` |
| DataRef 怎么存取 | `src/runtime/databus/databus.cpp:92-135` (store_with_ref / resolve_ref) |
| 容量监控接口 | `src/runtime/databus/databus.cpp:153-163` (get_used/total/count) |

### 2.3 分层职责

| 层 | 类 | 职责 |
|----|-----|------|
| **统一接口** | `MMDataBus` | 对外提供 `put/get/remove/store_with_ref/resolve_ref` 等操作，内部委托 local + remote |
| **本地存储** | `LocalDatabus` | 组合 `LocalChannel`（消息队列）和 `ZeroCopyDataBus`（KV 存储） |
| **共享内存** | `ZeroCopyDataBus` → `SharedMemoryBlock` | 有容量上限的 KV 存储，写超限返回 `false` 并打印 `"Shared memory full"` |
| **远程同步** | `RemoteDatabus` | 可选的 Redis 后端，提供跨节点数据同步和 DataRef 索引查询 |

### 2.3 SharedMemoryBlock 实现选择

`SharedMemoryFactory::create_best(capacity)` 按以下优先级选择实现：

```
1. create_in_process(capacity)     → InProcessSharedMemoryBlock (堆内存)
2. create_posix_shared(capacity)   → shm_open + mmap (当前回退到 in_process)
3. create_systemv_shared(capacity) → shmget + shmat (当前回退到 in_process)
```

> **注意**：POSIX 和 SysV 路径在当前实现中**总是回退到 `InProcessSharedMemoryBlock`**，因为跨进程布局尚未实现（源码注释：`cross-process layout is not yet implemented`）。实际存储始终是堆上的 `std::map<std::string, DataVariant>`。

---

## 3. 双总线模型

### 3.1 问题陈述

AgentFlow 运行时中存在**两个独立的 DataBus 实例**，服务于不同用途，但共享相同的类结构。这是生产环境 "Shared memory full" 错误的根因。

### 3.2 总线 A：Runtime Per-Agent DataBus

```
创建路径: MMAgentRuntime::register_agent() / get_databus()
配置来源: agent.json → data_bus_capacity_mb
生命周期: 与 Agent 注册周期一致
容量:     可配置（默认 1MB，outfit-assistant 配置为 500MB）
清理:     有 session cleanup callback
日志:     "DataBus initialized for agent_id=XXX, capacity=XXX bytes (XXX MB)"
```

**用途**：节点执行期间的业务数据读写（coordinator decisions、skill results、conversation history、LLM traces 等）。

**访问方式**：
- C++ 节点：通过 `agent->get_runtime_databus()` 直接访问
- Python 节点：通过 UDS Server 间接访问（`runtime.get_databus(agent_id)` 查找）

### 3.3 总线 B：DataRef Bridge 全局静态 DataBus

```
创建路径: dataref_bridge::control_databus() (函数内 static)
配置来源: 硬编码（原值 4MB）
生命周期: 进程级静态，永不销毁
容量:     硬编码（原值 4 * 1024 * 1024 = 4MB）
清理:     ❌ 无清理机制
日志:     "[ZeroCopyDataBus] Initialized with capacity=XXX bytes (XXX MB)"
          "RemoteDatabus connected to redis://127.0.0.1:6379"
```

**用途**：Task 间消息传递的 payload 存储。消息队列（`msgque`）只传 DataRef 地址，实际数据存在这里。

**访问方式**：
- `dataref_bridge::store_payload()` — 存入 payload，返回 DataRef 地址
- `dataref_bridge::resolve_payload()` — 通过 DataRef 地址取回 payload

### 3.4 双总线对比

| 维度 | Runtime DataBus (总线 A) | DataRef Bridge (总线 B) |
|------|-------------------------|------------------------|
| **实例数量** | 每个 agent 一个 | 全局唯一（static） |
| **容量配置** | agent.json `data_bus_capacity_mb` | 硬编码（原 4MB） |
| **清理机制** | session aging callback | ❌ 无 |
| **Redis 连接** | 可选 | 默认连接 `redis://127.0.0.1:6379` |
| **日志标识** | `DataBus initialized for agent_id=...` | 无 agent_id 关联 |
| **数据内容** | 业务数据（LLM trace、context、skill result） | 消息 payload（用户输入、LLM response、控制消息） |
| **容量耗尽影响** | 节点写入失败，数据丢失 | 消息 payload 写入失败，消息传递中断 |

### 3.5 数据流全景

#### 控制面 → 数据面的完整桥梁

消息从 msgque 出队后，框架通过**两条并行通道**将 payload 传递给节点执行环境：

```
用户请求: "推荐穿搭"
    │
    ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  ⓐ 控制面：msgque 消息队列                                               │
│                                                                          │
│  enqueue: payload → store_payload() → 总线 B → 返回 data_ref            │
│           message = { data_ref: "databus://global/abc123" } (轻量)      │
│                                                                          │
│  dequeue: message.data_ref → resolve_payload() → 总线 B → 还原 payload  │
│           message.content = "推荐穿搭" (已还原)                           │
└──────────────────────────────┬──────────────────────────────────────────┘
                               │
                    message.content + message.data_ref
                               │
    ┌──────────────────────────┼──────────────────────────┐
    │                          │                          │
    ▼                          ▼                          ▼
┌──────────────┐    ┌─────────────────────┐    ┌──────────────────────┐
│  ⓑ 桥梁 1:    │    │  ⓑ 桥梁 2:           │    │  ⓑ 桥梁 3:            │
│  环境变量注入  │    │  写入总线 A          │    │  DataRef 传递         │
│              │    │                     │    │                      │
│ build_runtime│    │ agent.cpp:1389      │    │ task->set_parameter( │
│ _task():     │    │ publish_data(       │    │   "message_data_ref",│
│ set_parameter│    │   "{dialogue_id}/   │    │   payload_ref)       │
│ ("message_   │    │    user_prompt",    │    │                      │
│  content",   │    │   user_text)        │    │ 节点可通过 data_ref   │
│  payload)    │    │                     │    │ 按需从总线 B 拉取      │
│      ↓       │    │      ↓              │    │                      │
│ worker.cpp:  │    │ ┌── 总线 A ────┐    │    └──────────────────────┘
│ 170:         │    │ │ user_prompt  │    │
│ "AF_MESSAGE_ │    │ │ = "推荐穿搭"  │    │
│  CONTENT=" + │    │ └──────────────┘    │
│  payload     │    │                     │
│              │    │ (节点通过 UDS 读取)  │
└──────┬───────┘    └──────────┬──────────┘
       │                       │
       ▼                       ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  ⓒ 数据面：节点执行环境 (Python 子进程)                                   │
│                                                                          │
│  coordinator_decide.py / skill_executor.py:                              │
│                                                                          │
│  通道 1 (环境变量 — 直接可用):                                            │
│    user_input = os.environ.get("AF_MESSAGE_CONTENT", "")                 │
│    → "推荐穿搭"                                                           │
│                                                                          │
│  通道 2 (总线 A — 通过 UDS 间接读取):                                     │
│    user_prompt = sys().get("{dialogue_id}/user_prompt")                  │
│    → "推荐穿搭"  (通过 UDS socket → runtime.get_databus → 总线 A)        │
│                                                                          │
│  通道 3 (总线 B — 通过 DataRef 按需拉取):                                 │
│    data_ref = os.environ.get("AF_MESSAGE_DATA_REF", "")                  │
│    payload = dataref_bridge.resolve_payload(data_ref)  # C++ 侧完成      │
│    → "推荐穿搭"  (通常由框架在 ⓑ 阶段已解析，节点不需要直接调用)          │
│                                                                          │
│  节点执行业务逻辑:                                                       │
│    sys().put("/session/{sid}/llm/traces/...", trace_data)  → 总线 A      │
│    sys().put_global("dispatch:1", manifest)                 → 总线 A      │
│    sys().put("skill_result:skill-runner", result)           → 总线 A      │
│                                                                          │
│  session 老化 → cleanup callback → remove_by_prefix("/session/{sid}/")   │
└─────────────────────────────────────────────────────────────────────────┘
```

#### 三条通道的关系

| 通道 | 传输方式 | 数据来源 | 节点读取方式 | 适用场景 |
|------|---------|---------|-------------|---------|
| **① 环境变量** | worker 注入 `AF_MESSAGE_CONTENT` | 框架从 msgque 还原 | `os.environ["AF_MESSAGE_CONTENT"]` | 最快，节点启动即可用 |
| **② 总线 A** | `publish_data("{dialogue}/user_prompt")` | 框架写入 | `sys().get("{dialogue}/user_prompt")` via UDS | 跨节点共享（forward 场景恢复原始输入） |
| **③ DataRef** | `task->set_parameter("message_data_ref")` | 总线 B 的 DataRef 地址 | 通常由框架解析，节点不直接使用 | 大 payload 按需拉取（避免环境变量超长） |

> **关键设计**：通道 ② 是 `dummy_pre_skill → coordinator` forward 机制的基础。框架 forward 时不从消息恢复 content，而是直接从总线 A 的 `{dialogue_id}/user_prompt` 读取原始输入（agent.cpp:1723-1726、1994-1997）。

---

## 4. 实例生命周期管理

### 4.1 总线 A 创建路径（3 条）

所有路径均打印日志：`"DataBus initialized for agent_id=XXX, capacity=XXX bytes (XXX MB)"`

#### 路径 1：register_agent() — 运行时未启动

```cpp
// runtime.cpp:328-334
bool MMAgentRuntime::register_agent(AgentPtr agent) {
    if(!is_running_) {
        agents_[agent->get_id()] = agent;
        size_t capacity = agent->get_config().data_bus_capacity_mb * 1024 * 1024;
        databuses_[agent->get_id()] = new MMDataBus(capacity);  // ← 创建
    }
}
```

**触发时机**：daemon 启动时加载 agent 包，runtime 尚未 start。

#### 路径 2：注册队列处理 — 运行时已启动

```cpp
// runtime.cpp:1811-1815 (异步注册队列消费线程)
agents_[agent_id] = request.agent;
size_t capacity = request.agent->get_config().data_bus_capacity_mb * 1024 * 1024;
databuses_[agent_id] = new MMDataBus(capacity);  // ← 创建
```

**触发时机**：运行时已启动后动态注册 agent（热加载）。

#### 路径 3：get_databus() — 懒创建

```cpp
// runtime.cpp:2312-2326
MMDataBus& get_databus(const AgentId& agent_id) {
    auto it = databuses_.find(agent_id);
    if(it == databuses_.end()) {
        // DataBus 不存在 → 查 agent 配置或用默认值
        size_t capacity = 1024 * 1024;  // 默认 1MB
        auto agent_it = agents_.find(agent_id);
        if(agent_it != agents_.end()) {
            capacity = agent_it->second->get_config().data_bus_capacity_mb * 1024 * 1024;
        }
        databuses_[agent_id] = new MMDataBus(capacity);  // ← 创建
    }
}
```

**触发时机**：使用未注册的 `agent_id` 查找 DataBus。

> ⚠️ **风险**：如果请求中的 `agent_id` 与注册时的不一致（大小写、命名差异），会触发此路径创建一个默认容量的新 DataBus。

### 4.2 总线 B 创建路径（1 条）

```cpp
// dataref_bridge.cpp:11-18
runtime::MMDataBus& control_databus() {
    static runtime::MMDataBus bus(4 * 1024 * 1024);  // ← 硬编码 4MB
    static std::once_flag once;
    std::call_once(once, [] {
        bus.connect_remote("redis://127.0.0.1:6379");
    });
    return bus;
}
```

**触发时机**：首次调用 `store_payload()` / `resolve_payload()` / `find_refs_by_owner()` 时（C++ static 局部变量初始化）。

**特点**：
- 进程级单例，`std::call_once` 保证只初始化一次
- 容量硬编码，**不受 agent.json 配置影响**
- 自动连接本地 Redis（连接失败不影响功能，仅降级为纯本地）

### 4.3 销毁时机

| 实例 | 销毁时机 |
|------|---------|
| 总线 A (register_agent) | `unregister_agent()` 时 `databuses_.erase(agent_id)` |
| 总线 A (懒创建) | 同上，或进程退出 |
| 总线 B (dataref_bridge) | ❌ 永不销毁（进程级 static） |

---

## 5. 数据操作模式

### 5.1 基本 KV 操作

```cpp
// 写入（返回 false 表示容量不足）
bool MMDataBus::put(key, value);

// 读取（本地 miss 时自动从 Redis 拉取并回填）
bool MMDataBus::get(key, value);

// 删除
bool MMDataBus::remove(key);

// 前缀批量操作
int MMDataBus::remove_by_prefix(prefix);
std::vector<std::string> MMDataBus::list_keys_by_prefix(prefix);
```

### 5.2 容量检查逻辑

```cpp
// InProcessSharedMemoryBlock::write()
if(used_capacity_ - old_size + total_size > capacity_) {
    Logger::error_f("Shared memory full: used=%zu, freed=%zu, need=%zu",
                    used_capacity_, old_size, total_size);
    return false;  // 写入失败，数据被丢弃
}
```

**容量计算**：`used_capacity_` = 所有 key.size() + estimate_variant_size(value) 之和。

### 5.3 本地/远程读写策略

```
put(key, value):
    1. local_bus_->put(key, value)          // 总是写本地
    2. if redis connected:
           remote_bus_->publish(key, value) // 可选写 Redis

get(key, value):
    1. local_bus_->get(key, value)          // 先读本地
    2. if local miss && redis connected:
           remote_bus_->fetch(key, value)   // 读 Redis
           local_bus_->put(key, value)      // 回填本地缓存
```

> ⚠️ **注意**：`get()` 的回填行为会**增加**本地 DataBus 的 used_capacity。如果 Redis 中有大量历史数据，本地缓存可能被回填数据撑满。

### 5.4 订阅机制

```cpp
// 订阅 key 变化
void MMDataBus::subscribe(key, callback);

// 触发时机：put() 成功后通知所有订阅者
bool MMDataBus::put(key, value) {
    if(local_bus_->put(key, value)) {
        notify_subscribers(key, value);  // ← 同步回调
        return true;
    }
}
```

---

## 6. DataRef 与控制面/数据面分离

### 6.1 设计意图

Task 间消息（dispatcher → coordinator → skill）需要传递大量 payload（用户输入、LLM response、skill result）。如果消息队列直接携带 payload：

- 消息体过大，序列化/反序列化开销高
- 消息队列内存占用不可控
- 无法跨进程/跨节点共享

**DataRef 模式**：消息只传地址（`databus://global/<data_id>`），payload 存在 dataref_bridge 的 DataBus 中。

#### DataRef 不是瞬态数据——三个使用层次

从 Owner 索引（10 个字段）、Redis 跨节点共享、`find_refs_by_owner` 查询接口等设计看，DataRef 的意图是**多 agent 共享的可追溯存储**，而非简单的消息信封：

| 设计能力 | 代码证据 | 瞬态信封需要吗？ |
|---------|---------|----------------|
| Owner 归属追踪（10 字段） | `OwnerContext`: request_id/user_id/client_id/agent_id/session_id/task_id/workflow_id/node_id/master_id/worker_id | ❌ |
| Redis 跨节点共享 | `connect_remote("redis://...")` | ❌ |
| 按维度查询 | `find_refs_by_owner("user_id", "alice")` | ❌ |
| 内容去重 | `build_data_id(payload)` = hash(content + timestamp) | ❌ |

DataRef 的设计覆盖三个递进的使用层次：

| 层次 | 场景 | DataRef 生命周期 | 说明 |
|------|------|-----------------|------|
| **1. 消息信封** | 单 agent 内 task 间传递 | enqueue → dequeue → 即可丢弃 | 最基础用法（outfit-assistant 现状） |
| **2. 多 agent 共享** | 同一 daemon 内 agent A 的结果被 agent B 查询 | session 级保留 | 多 agent 协作的基础 |
| **3. 跨 daemon 分布式** | 多个 daemon 通过 Redis 共享数据 | 需 TTL 或引用计数 | AgentFlow 的跨节点多 agent 协作愿景 |

> **关键约束**：任何对 DataRef 存储的修改（清理策略、容量配置、key 格式变更）都**不能破坏层次 2/3 的能力**。当前 outfit-assistant 只使用层次 1，但框架按层次 2/3 的愿景设计了 DataRef 的 owner 索引、Redis 同步和查询接口。

### 6.2 DataRef 生命周期

```
存储: store_payload(payload, metadata, logical_key)
      → 生成 data_id = hash(payload + timestamp)
      → 存入 dataref_bridge DataBus (key = "data_ref:{data_id}")
      → 存入 dataref_bridge DataBus (key = logical_key)
      → 如果 Redis 连接: 建立 owner 索引
      → 返回 address = "databus://global/{data_id}"

解析: resolve_payload(data_ref, payload)
      → 解析 data_ref 提取 data_id
      → 从 dataref_bridge DataBus 读取 "data_ref:{data_id}"
      → 本地 miss 时从 Redis 拉取并回填
```

### 6.3 Owner 索引

```cpp
// store_with_ref 写入时同时建立 owner 索引到 Redis
remote_bus_->index_data_ref("session_id:abc123", "databus://global/xyz789");
remote_bus_->index_data_ref("user_id:user1", "databus://global/xyz789");
remote_bus_->index_data_ref("agent_id:outfit-assistant", "databus://global/xyz789");

// 查询某 owner 下的所有 DataRef
find_refs_by_owner("session_id", "abc123")
    → ["databus://global/xyz789", "databus://global/def456", ...]
```

### 6.4 消息传递中的 DataRef 流

```
dispatcher task                        coordinator task
┌──────────────┐                      ┌──────────────────┐
│ 用户输入      │                      │                  │
│ "推荐穿搭"    │                      │                  │
│      ↓       │                      │                  │
│ store_payload│                      │                  │
│ ("推荐穿搭")  │                      │                  │
│      ↓       │                      │                  │
│ data_ref =   │  ──── 消息队列 ────→  │ message.data_ref │
│ "databus://  │  只传 data_ref       │      ↓           │
│  global/abc" │  (很轻量)            │ resolve_payload  │
│              │                      │ ("databus://     │
│              │                      │  global/abc")    │
│              │                      │      ↓           │
│              │                      │ payload =        │
│              │                      │ "推荐穿搭"        │
└──────────────┘                      └──────────────────┘
```

### 6.5 调用方清单

| 文件 | 行 | 操作 | 场景 |
|------|-----|------|------|
| `agent.cpp` | 1245 | `store_payload` | Agent 消息发送时存储 payload |
| `agent.cpp` | 1247 | `resolve_payload` | Agent 消息接收时还原 payload |
| `agent.cpp` | 1253 | `store_payload` | 另一处 payload 存储 |
| `msgque.cpp` | 63 | `store_payload` | 控制消息发送时存储 content |
| `msgque.cpp` | 83 | `resolve_payload` | 控制消息接收时还原 content |
| `context.cpp` | 129 | `store_payload` | 上下文数据存储 |
| `coordinator.cpp` | 132 | `resolve_payload` | Coordinator 解析收到的 data_ref |

### 6.6 Task 间数据流实例（outfit-assistant）

以 outfit-assistant 为例，展示一次完整请求中 Task 之间的消息如何通过两条总线流转。

#### Agent 配置（4 个 Task）

```json
"tasks": [
    { "id": 0, "type": "dispatcher" },
    { "id": 1, "type": "coordinator" },
    { "id": 2, "type": "skill", "name": "dummy_pre_skill" },
    { "id": 3, "type": "skill", "name": "skill-runner" }
]
```

#### 用户输入："推荐一套商务休闲穿搭"

```
用户请求: "推荐一套商务休闲穿搭"
    │
    │  ① daemon 收到请求，构造 Message(content="推荐一套商务休闲穿搭")
    │
    ▼
┌─────────────────────────────────────────────────────────────┐
│  enqueue_message → dispatcher task 的消息队列                │
│                                                              │
│  msgque.cpp:59-66:                                           │
│    content 不为空 → store_payload("推荐一套商务休闲穿搭")      │
│    ↓                                                         │
│    总线 B (dataref_bridge): 写入 payload                      │
│    返回 data_ref = "databus://global/abc123"                 │
│    message.content 清空                                       │
│    message.data_ref = "databus://global/abc123"              │
│                                                              │
│  消息队列里只存: { data_ref: "databus://global/abc123" }     │
└──────────────────────────┬──────────────────────────────────┘
                           │
    │  ② dispatcher task 取出消息
    │
    ▼
┌─────────────────────────────────────────────────────────────┐
│  dequeue_message 从队列取出                                   │
│                                                              │
│  msgque.cpp:82-83:                                           │
│    message.content 为空 → resolve_payload(data_ref)          │
│    ↓                                                         │
│    总线 B (dataref_bridge): 读取 payload                      │
│    message.content = "推荐一套商务休闲穿搭" (还原)             │
│                                                              │
│  dispatcher 逻辑: next_skills=["dummy_pre_skill"]            │
│  → 写 user_prompt 到 总线 A                                   │
│    sys().put("/session/{sid}/user_prompt", "推荐一套…")      │
└──────────────────────────┬──────────────────────────────────┘
                           │
    │  ③ dispatcher → dummy_pre_skill task（跨 task 消息）
    │     新消息 → 再次走 msgque → 总线 B 存 payload
    │
    ▼
┌─────────────────────────────────────────────────────────────┐
│  dummy_pre_skill task 执行                                    │
│  纯 passthrough，不处理，完成后 framework 自动转发             │
└──────────────────────────┬──────────────────────────────────┘
                           │
    │  ④ framework forward → coordinator task（跨 task 消息）
    │     再次走 msgque → 总线 B 存 payload
    │
    ▼
┌─────────────────────────────────────────────────────────────┐
│  coordinator task 执行 (coordinator_decide.py)                │
│                                                              │
│  读 总线 A: sys().get("/session/{sid}/user_prompt")          │
│    → "推荐一套商务休闲穿搭"                                   │
│                                                              │
│  调 LLM → 返回: 调用 weather_query + outfit-advisor          │
│                                                              │
│  写 总线 A: sys().put_global("dispatch:1", manifest)         │
│    manifest = {skill:"weather_query", prompt:"查北京天气"}    │
│                                                              │
│  输出 next_skills: [{name:"skill-runner",                    │
│                       exec_skill_key:"dispatch:1"}]          │
└──────────────────────────┬──────────────────────────────────┘
                           │
    │  ⑤ coordinator → skill-runner task（跨 task 消息）
    │     走 msgque → 总线 B 存 payload（skill 调度信息）
    │
    ▼
┌─────────────────────────────────────────────────────────────┐
│  skill-runner task 执行 (skill_executor.py)                   │
│                                                              │
│  读 总线 A: AF_EXEC_SKILL_KEY="dispatch:1"                   │
│    → sys().get_global("dispatch:1") → manifest                │
│    → skill_name="weather_query", prompt="查北京天气"          │
│                                                              │
│  执行 weather_query skill:                                    │
│    读 agent.md → 构建 system prompt                           │
│    LLM while-loop → 调 weather tool → 29°C 多云               │
│                                                              │
│  写 总线 A: sys().put("skill_result:skill-runner",            │
│                        {天气数据 + 穿搭建议})                  │
└──────────────────────────┬──────────────────────────────────┘
                           │
    │  ⑥ skill-runner 完成 → coordinator 重新触发（跨 task）
    │     走 msgque → 总线 B 存 payload（skill 完成通知）
    │
    ▼
┌─────────────────────────────────────────────────────────────┐
│  coordinator 再次执行 (skill 完成触发)                        │
│                                                              │
│  读 总线 A: sys().get("skill_result:skill-runner")           │
│    → {29°C, 多云, 湿度58%, 穿搭建议}                          │
│                                                              │
│  读 总线 A: llm/session_messages → 对话历史                   │
│                                                              │
│  调 LLM → "根据天气29°C多云，推荐商务休闲穿搭…"                │
│                                                              │
│  写 总线 A: llm/session_messages → 追加对话历史               │
│  写 总线 A: llm/traces/{call_id}/... → trace 记录             │
│                                                              │
│  输出 response → 消息走 msgque → 总线 B 存 payload             │
└──────────────────────────┬──────────────────────────────────┘
                           │
    ▼
用户收到: "根据天气29°C多云，推荐商务休闲穿搭…"
```

#### 两条总线各管什么

| 场景 | 总线 | 数据内容 |
|------|------|---------|
| **Task 之间传消息** | **总线 B** (dataref_bridge) | 用户输入、skill 调度信息、skill 完成通知、最终 response |
| **节点执行时读写业务数据** | **总线 A** (runtime) | user_prompt、dispatch manifest、skill_result、LLM traces、对话历史 |

> **一句话**：Task 间的消息信封走总线 B，Task 内的业务数据走总线 A。

---

## 7. 容量配置体系

### 7.1 配置链路（总线 A）

```
agent.json
  └─ "data_bus_capacity_mb": 500
       └─ package.cpp:324-331 解析
            config.data_bus_capacity_mb = agent_doc["data_bus_capacity_mb"].GetInt()
            (默认值: 1, 必须为正整数)
            └─ agent.cpp:454 拷贝到 agent config
                 config.data_bus_capacity_mb = package->get_config().data_bus_capacity_mb
                 └─ runtime.cpp:331 转字节
                      size_t capacity = config.data_bus_capacity_mb * 1024 * 1024
                      └─ new MMDataBus(capacity)
                           └─ LocalDatabus(capacity)
                                └─ ZeroCopyDataBus(capacity)
                                     └─ InProcessSharedMemoryBlock(capacity)
                                          capacity_ = capacity  // 最终上限
```

### 7.2 配置值参考

| Agent | data_bus_capacity_mb | 实际容量 |
|-------|---------------------|---------|
| 默认值（未配置） | 1 | 1 MB |
| outfit-assistant | 500 | 500 MB |
| harness_agent (模板) | 50 | 50 MB |

### 7.3 总线 B 的配置缺口

```
dataref_bridge::control_databus()
  └─ static MMDataBus bus(4 * 1024 * 1024)
       ↑ 硬编码，不受任何配置影响
```

**问题**：
- 不读 agent.json
- 不读环境变量
- 不读命令行参数
- 不读任何配置文件

### 7.4 容量监控

```cpp
// 运行时可查询
size_t used  = databus.get_used_capacity();    // 已用字节
size_t total = databus.get_total_capacity();    // 总容量字节
double usage = databus.get_capacity_usage();    // 百分比 (0.0 ~ 100.0)
int    count = databus.get_data_count();        // KV 条目数
```

> **建议**：通过 daemon RPC 或日志定期输出这些指标，设置告警阈值（如 >80%）。

---

## 8. Session 清理机制

### 8.1 总线 A 的清理

Session 老化时触发 cleanup callback，清除该 session 的所有 DataBus keys：

```cpp
// agent.cpp:598-603
session_router_->set_session_cleanup_callback(
    [this](session_id, seq) {
        int cleared = clear_session_data_if_unused(session_id, seq);
        Logger::info("Cleared %d DataBus keys for aged-out session=%s", cleared, session_id);
    }
);
```

**清理内容**：`remove_by_prefix("/session/{session_id}/")` — 删除该 session 下所有业务数据。

**触发条件**：SessionRouter 的 aging_loop 定期检查，超过老化超时的 session 被回收。

### 8.2 总线 B 的清理

❌ **无清理机制**。

DataRef 数据存储后永不删除：
- `store_payload()` 只写不删
- 没有 session-level 的 DataRef 清理 callback
- 没有 TTL（生存时间）机制
- 没有 LRU（最近最少使用）淘汰机制

**影响**：长时间运行的 daemon，DataRef 数据持续累积，最终耗尽 4MB（或修改后的更大值）容量。

### 8.3 显式清理接口

daemon RPC 提供了显式清理接口：

```cpp
// rpc_handler.cpp:539
int removed = runtime_.clear_session_data(agent_id, session_id);
```

但这只清理总线 A 的数据，不清理总线 B 的 DataRef。

---

## 9. UDS 多语言访问

### 9.1 架构

```
Python 节点 (skill_executor.py / coordinator_decide.py)
    │
    │ Unix Domain Socket (JSON 协议)
    │
    ▼
UDSServer (C++)
    │
    │ runtime.get_databus(agent_id)
    │
    ▼
总线 A (Per-Agent MMDataBus)
```

### 9.2 UDS 协议

Python 端通过 `agentflow_databus` 模块发送 JSON 请求：

```json
// put 请求
{"op": "put", "key": "/session/abc/user_prompt", "value": "推荐穿搭"}

// get 请求
{"op": "get", "key": "/session/abc/user_prompt"}

// remove 请求
{"op": "remove", "key": "/session/abc/user_prompt"}
```

UDS Server 解析 JSON，调用 `runtime.get_databus(agent_id)` 获取 DataBus 实例，执行操作后返回结果。

### 9.3 agent_id 传递

UDS 请求中的 `agent_id` 来自 Python 节点的环境变量 `AF_AGENT_ID`。如果此值与注册时的 agent_id 不匹配，会触发 `get_databus()` 的懒创建路径，创建一个默认容量的 DataBus。

---

## 10. Redis 远程同步

### 10.1 连接配置

```
// 总线 A: 通过 MMDataBus::connect_remote(address) 显式连接
// 总线 B: 自动连接 "redis://127.0.0.1:6379"
```

### 10.2 Redis 操作映射

| DataBus 操作 | Redis 操作 |
|-------------|-----------|
| `put(key, value)` | `SET key value` |
| `get(key)` (local miss) | `GET key` |
| `remove(key)` | `DEL key` |
| `index_data_ref(index_key, ref)` | `SADD index_key ref` |
| `find_indexed_refs(index_key)` | `SMEMBERS index_key` |

### 10.3 Redis 不可用时的降级

`RemoteDatabus` 的所有操作在 `!connected_` 时直接返回 `false`/空，不影响本地 DataBus 功能。`MMDataBus` 的 `put()` 返回 `local_success`（只看本地写入结果）。

---

## 11. 已知问题与风险

### 11.1 P0：dataref_bridge 容量硬编码 + 无清理

| 项 | 详情 |
|----|------|
| **现象** | 生产环境出现 `"Shared memory full: used=4194283, freed=0, need=2955"` |
| **根因** | `dataref_bridge.cpp:12` 硬编码 `4 * 1024 * 1024`（4MB），不受 agent.json 配置影响 |
| **影响** | 累积约 4MB 消息 payload 后，所有新消息存储失败，消息传递中断 |
| **临时修复** | 增大硬编码值（如 222MB 或 512MB） |
| **长期方案** | (1) 从环境变量读取容量；(2) 添加 DataRef 清理机制（session 结束时清理）；(3) 考虑合并到 Runtime DataBus |

#### 容量消耗分析

用户输入本身很小（几十字节），但总线 B 存储的是**所有跨 task 消息的 payload**，其中 LLM 响应是容量消耗的大头：

| 消息类型 | 单条大小 | 说明 |
|---------|---------|------|
| 用户输入 | ~40 bytes | 很小 |
| dispatch manifest | ~500 bytes | 中等 |
| **skill 执行结果 (LLM)** | **~10-30 KB** | 含天气数据 + 穿搭建议 + LLM 推理过程 |
| **coordinator 最终回复 (LLM)** | **~5-20 KB** | 含评估结果 + 推理 + 最终建议 |
| **response 消息** | **~5-20 KB** | 最终给用户的完整回复 |

一次完整请求（coordinator → skill → coordinator 循环）在总线 B 中累积约 **70-80 KB**（含 §11.2 的双份存储放大）。

4MB 容量约 **50 次请求即满**；如果 LLM 响应更长（带详细推理），可能 **20-30 次即满**。

### 11.2 P0：store_with_ref 双份存储浪费 50% 容量

| 项 | 详情 |
|----|------|
| **现象** | `store_with_ref()` 将同一 payload 存两份到 DataBus |
| **根因** | `databus.cpp:103-107`，详见下方代码 |
| **影响** | 有效容量减半（4MB 实际只存 ~2MB 数据）；加速 "Shared memory full" 触发 |
| **优先级** | P0 — 与 §11.1 叠加，是容量耗尽的直接放大因素 |

**问题代码** (`src/runtime/databus/databus.cpp:103-107`)：

```cpp
bool MMDataBus::store_with_ref(const std::string& key, const DataVariant& value, ...) {
    // ...
    const std::string data_key = "data_ref:" + data_id;

    // 第一份：按 DataRef 地址存储（resolve_ref 读取这个）
    if(!put(data_key, value)) {         // ← 检查返回值
        return false;
    }

    // 第二份：按逻辑 key 存储（没人读这个）
    put(key, value);                    // ← 返回值被忽略！

    // ...
    return true;
}
```

**只有第一份被读取**：

```cpp
bool MMDataBus::resolve_ref(const std::string& data_ref, DataVariant& value) const {
    const std::string data_id = data_ref.substr(prefix.size());
    const std::string data_key = "data_ref:" + data_id;
    return get(data_key, value);        // ← 只读 data_ref:{hash}，从不读逻辑 key
}
```

`find_refs_by_owner` 走 Redis 索引，不读本地逻辑 key。**没有任何代码通过 `get("msg:{message_id}")` 读取第二份数据。**

**三个问题**：

| 问题 | 说明 |
|------|------|
| **浪费 50% 容量** | 同一 payload 存两份，有效容量减半 |
| **返回值被忽略** | `put(key, value)` 第二次写入失败时静默忽略，调用方认为成功但数据不完整 |
| **永不清理** | 两份副本都没有清理机制，只增不减 |

**推测原因**：注释 `// Cache ref by key for quick lookups` 表明开发者计划支持两种查找路径（地址查找 + 逻辑 key 查找），但逻辑 key 查找的调用方从未实现，形成了半成品设计。

**修复方案**：

```cpp
// 方案 A：去掉第二份存储（推荐，改动最小，立即释放 50% 容量）
bool MMDataBus::store_with_ref(const std::string& key, const DataVariant& value, ...) {
    const std::string data_key = "data_ref:" + data_id;
    if(!put(data_key, value)) {
        return false;
    }
    // 删除: put(key, value);  ← 不再存第二份
    key_to_ref_[key] = out_ref;  // 内存映射保留，不占 DataBus 容量
    // ...owner index...
    return true;
}

// 方案 B：如果确实需要逻辑 key 查找，只存地址引用而非完整 payload
put(key, out_ref.address);  // 存 ~50 bytes 地址，而非 ~10-30KB 完整 payload
```

### 11.2 P1：双总线架构风险

| 项 | 详情 |
|----|------|
| **现状** | 两个独立 MMDataBus 实例，配置/清理/监控各自为政 |
| **风险** | 运维人员只监控总线 A 的容量，不知道总线 B 的存在 |
| **文档** | enhancement 032 (`tbd`) 已识别此风险，但尚未推进实施 |
| **建议** | 短期：统一监控两个实例的容量；长期：评估是否合并为单一 DataBus |

### 11.3 P1：get() 回填导致容量膨胀

| 项 | 详情 |
|----|------|
| **现象** | `get()` 本地 miss 时从 Redis 拉取并回填本地缓存 |
| **风险** | Redis 中的历史数据被回填到本地 DataBus，增加 used_capacity |
| **影响** | 即使 session cleanup 清理了本地数据，后续 `get()` 可能从 Redis 回填回来 |
| **建议** | 回填时检查容量上限，或在 session cleanup 时同时清理 Redis 对应 key |

### 11.4 P2：get_databus() 懒创建 agent_id 不匹配

| 项 | 详情 |
|----|------|
| **现象** | 用未注册的 agent_id 查找 DataBus，会创建默认 1MB 的实例 |
| **触发条件** | agent_id 大小写不一致、命名差异、或 Python 节点传错 agent_id |
| **影响** | 业务请求命中 1MB DataBus 而非配置的 500MB 实例 |
| **建议** | (1) 懒创建时打印 WARN 日志；(2) agent_id 查找时做规范化（大小写不敏感或别名映射） |

### 11.5 P2：SharedMemoryBlock 跨进程未实现

| 项 | 详情 |
|----|------|
| **现状** | POSIX/SysV SHM 路径总是回退到 `InProcessSharedMemoryBlock` |
| **影响** | 无法实现真正的跨进程共享内存（多 worker 进程场景） |
| **注释** | 源码标注 `cross-process layout is not yet implemented` |

---

## 12. 关键文件索引

### 12.1 核心实现

| 文件 | 说明 |
|------|------|
| `include/internal/runtime/databus/databus.h` | `MMDataBus` 类定义（统一接口） |
| `src/runtime/databus/databus.cpp` | `MMDataBus` 实现（local/remote 委托、DataRef） |
| `include/internal/runtime/databus/local_databus.h` | `LocalDatabus`、`ZeroCopyDataBus`、`SharedMemoryBlock` 类定义 |
| `src/runtime/databus/local_databus.cpp` | 本地存储实现（InProcessSHM、Factory、容量检查） |
| `src/runtime/databus/remote_databus.cpp` | `RemoteDatabus` 实现（Redis 适配） |
| `src/runtime/databus/uds_server.cpp` | UDS Server（多语言 DataBus 访问） |

### 12.2 集成层

| 文件 | 说明 |
|------|------|
| `include/internal/agent/dataref_bridge.h` | DataRef Bridge 接口 |
| `src/agent/dataref_bridge.cpp` | **全局静态 4MB DataBus 实例**（总线 B 根因） |
| `src/runtime/runtime.cpp` | Runtime DataBus 创建/管理（总线 A 的 3 条创建路径） |
| `src/agent/agent.cpp` | Session cleanup callback 注册 |
| `src/agent/session_router.cpp` | Session 老化检测与 cleanup 触发 |

### 12.3 配置解析

| 文件 | 行 | 说明 |
|------|-----|------|
| `src/agent/package.cpp` | 324-331 | 从 agent.json 解析 `data_bus_capacity_mb` |
| `src/workflow/dsl/workflow_parser.cpp` | 463-465 | DSL 解析器中的 `data_bus_capacity_mb` |
| `src/workflow/dsl/dsl_adapter.cpp` | 995 | DSL 适配器中的 `data_bus_capacity_mb` |
| `src/agent/agent.cpp` | 454 | 从 package config 拷贝到 agent config |

### 12.4 消费者

| 文件 | 说明 |
|------|------|
| `src/agent/msgque.cpp` | 消息队列中使用 DataRef 存储/还原 payload |
| `src/agent/context.cpp` | 上下文数据通过 DataRef 存储 |
| `src/agent/coordinator.cpp` | Coordinator 解析收到的 DataRef |
| `src/daemon/rpc_handler.cpp` | daemon RPC 代理 DataBus 操作（put/get/list/clear） |

### 12.5 Python 客户端

| 文件 | 说明 |
|------|------|
| `src/runtime/databus/clients/python/agentflow_databus.py` | Python DataBus 客户端（通过 UDS） |
| `examples/agent/outfit-assistant/nodes/databus_client.py` | outfit-assistant 封装的 DataBus 客户端 |

### 12.6 测试

| 文件 | 说明 |
|------|------|
| `tests/unit/src/test_databus.cpp` | DataBus 单元测试 |
| `tests/unit/src/test_databus_gtest.cpp` | GoogleTest DataBus 测试 |
| `tests/unit/src/test_databus_concurrent.cpp` | 并发安全测试 |
| `tests/unit/src/test_databus_session_isolation.cpp` | Session 隔离测试 |
| `tests/e2e/databus_cleanup/test_databus_cleanup_e2e.cpp` | E2E 清理测试 |
| `examples/agent/outfit-assistant/tests/test_databus_memory_regression.py` | Python 回归测试 |

### 12.7 相关文档

| 文件 | 状态 | 说明 |
|------|------|------|
| `docs/ARCHITECTURE.md` | 已实现 | 架构总览（含 DataRef 简述） |
| `docs/enhancements/003-refactor-mmdatabus-全局共享重构-260222-implemented.md` | `implemented` | DataRef 引入 |
| `docs/enhancements/032-feature-dispatcher-coordinator-databus-observability-260405-tbd.md` | `tbd` | 双总线风险识别 |
| `docs/databus_user_manual.md` | 已实现 | DataBus 使用手册 |
| `docs/api-reference.md` | 已实现 | API 参考（含 DataRef） |

---

## 附录 A：错误诊断指南

### "Shared memory full" 错误排查

1. **确认是哪个总线**：
   ```bash
   # 查看所有 DataBus 实例
   grep "DataBus initialized for agent_id" daemon.log
   grep "ZeroCopyDataBus.*Initialized" daemon.log
   ```

2. **如果是总线 A（agent DataBus）**：
   - 检查 agent.json 的 `data_bus_capacity_mb` 是否够大
   - 检查 session cleanup 是否正常工作（搜索 `"Cleared.*DataBus keys"` 日志）

3. **如果是总线 B（dataref_bridge）**：
   - 检查 `dataref_bridge.cpp` 中的硬编码容量
   - 长期运行需要考虑添加 DataRef 清理机制

4. **错误参数解读**：
   ```
   "Shared memory full: used=X, freed=Y, need=Z"
   used = 当前已用容量（字节）
   freed = 本次写入可释放的旧值大小（0 = 新 key）
   need = 本次写入需要的新空间
   capacity ≈ used + need（当 freed=0 时）
   ```
