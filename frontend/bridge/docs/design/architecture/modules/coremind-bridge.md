# CoreMind Bridge — C++ SideCar 平台接入设计

> **层级**：L4 · `architecture/modules/coremind-bridge.md`
> **状态**：Implemented · 2026-07-03
> **上位决策**：[ADR-0005](../../adrs/0005-coremind-service-discovery.md)

---

## 1. 范围与状态

### 1.1 目标链路

```text
用户 / CoreMind
      ↓ HTTP
C++ SideCar（唯一外部入口 + 服务发现）
      ↓ 可信内部请求
Python Bridge
      ↓ agent.execute
AgentFlow daemon
      ↓
Workflow / Node
```

本模块负责：

- 接收 CoreMind 外部请求；
- 在转发前解析本次请求所需的固定白名单服务；
- 构造可信内部上下文并转发到 Python Bridge；
- 提供健康检查、能力宣告和 Agent 管理兼容端点。

本模块不负责：

- 业务编排和 Node 执行；
- 让 Bridge 反向调用 SideCar 获取服务发现；
- 缓存或刷新服务发现结果；
- 接受用户输入的任意服务名并执行发现。

### 1.2 实现状态

| 项目 | 实现状态 | 代码落点 |
|------|----------|----------|
| SideCar → Bridge `:8199` | 已实现 | `coremind_bridge/main.cpp` |
| 请求级服务快照 | 已实现 | `coremind_bridge/main.cpp` |
| Bridge 内部入口 | 已实现 | `routes/coremind_internal.py` |
| 内部来源与共享凭据校验 | 已实现 | `routes/coremind_internal.py`、`start.sh` |
| AgentFlow → Node 参数传播 | 已实现 | `rpc_handler.cpp`、`agent.cpp`、`runtime.cpp` |
| 完整 E2E | 已实现 | `test_coremind_sidecar_e2e.py#test_user_request_reaches_node_with_trusted_context` |

## 2. 核心设计决策

### 2.1 单向依赖

SideCar 在转发业务请求前完成服务发现，并将结果作为请求级快照交给 Bridge。
Bridge 不反向调用 SideCar，避免形成以下运行时依赖环：

```text
SideCar 等待 Bridge 响应
   ↑                ↓
   └── Bridge 同步请求 SideCar ──┘
```

依赖方向始终为：

```text
SideCar → Bridge → AgentFlow → Node
```

### 2.2 请求级快照，不设计 TTL

服务发现结果只属于当前请求，具有以下语义：

- SideCar 每次接收新请求时重新发现所需服务；
- Bridge 和 AgentFlow 不跨请求缓存该结果；
- 上下文中不包含 TTL，也不声明运行期间可刷新；
- Node 使用快照连接目标服务失败时，本次请求明确失败；
- 后续新请求重新执行服务发现，获得新的快照。

不采用 TTL 的原因：当前架构没有快照刷新通道。设置推测性的过期时间只会产生
“已过期但无法更新”的状态，不能提供真实的一致性保证。

### 2.3 服务白名单

SideCar 只能解析部署配置中声明的服务，不从用户请求读取服务名。初始白名单由部署配置给出，
例如 `IMInferScheduler`、`DMContextService`、`KMMService`。

配置必须区分：

- `required_services`：任一发现失败，本次请求不转发，SideCar 返回明确错误；
- `optional_services`：允许缺失，缺失项以结构化错误写入上下文。

具体服务清单属于部署配置，不在本文写死为产品承诺。

## 3. 可信内部上下文

### 3.1 传输边界

SideCar 不把平台上下文混入外部 AccessCtrl 载荷，也不使用可由外部调用方伪造的普通业务 Header。
它将外部请求转换为内部 envelope，并发送到 Bridge 专用端点：

```text
POST /internal/coremind/v1/callback
Content-Type: application/json
```

该端点只允许 SideCar 所在的内部网络边界访问，不通过 SideCar 的外部路由再次暴露。
Bridge 必须同时校验内部访问边界和共享凭据，任一校验失败均返回 `401/403`，且不得进入 AgentFlow。

### 3.2 Envelope 契约

```json
{
  "schema_version": "1.0",
  "request_id": "01J...",
  "source": "coremind-sidecar",
  "original_request": {
    "Header": {},
    "Data": {},
    "Payload": {}
  },
  "platform_context": {
    "services": {
      "IMInferScheduler": {
        "status": "resolved",
        "instances": [
          {"host": "10.0.0.5", "port": 27321}
        ]
      },
      "KMMService": {
        "status": "unavailable",
        "instances": [],
        "error_code": "SERVICE_DISCOVERY_EMPTY"
      }
    }
  }
}
```

字段规则：

| 字段 | 规则 |
|------|------|
| `schema_version` | 必填；Bridge 只接受明确支持的版本 |
| `request_id` | 由 SideCar 生成或采用已验证的上游追踪 ID；贯穿 Bridge、AgentFlow 与 Node |
| `source` | 固定为 `coremind-sidecar`，但不能单独作为鉴权依据 |
| `original_request` | 原始业务载荷；SideCar 不改变其业务字段 |
| `platform_context.services` | 只包含部署白名单内的服务 |
| `status` | `resolved` 或 `unavailable` |
| `instances` | `resolved` 时至少一个；保留 CoreMind 返回顺序 |
| `error_code` | `unavailable` 时必填；不得暴露底层凭据或敏感错误文本 |

上下文是内部运行时输入，不是最终用户响应。Bridge 将其放入本次 AgentFlow 执行上下文，
Node 只能读取，不得修改或回写服务目录。

### 3.3 信任不变量

- T1：只有通过内部端点鉴权的请求可以携带 `platform_context`。
- T2：外部请求中的同名字段必须被忽略，不得覆盖 SideCar 生成的上下文。
- T3：服务名只能来自 SideCar 部署白名单。
- T4：上下文仅绑定一个 `request_id` 和一次 AgentFlow 执行，不跨请求复用。
- T5：日志可记录服务名、状态和实例数量，不记录共享凭据。

## 4. 处理流程与失败语义

### 4.1 正常流程

1. SideCar 接收并校验外部请求。
2. SideCar 根据入口对应的依赖策略解析 required/optional 服务。
3. required 服务全部成功后，SideCar 构造内部 envelope。
4. Bridge 校验内部来源、凭据和 Schema。
5. Bridge 将原始业务请求与只读 `platform_context` 映射到 AgentFlow 执行上下文。
6. AgentFlow 调度 Workflow/Node，Node 按服务名读取实例。
7. 结果沿 Bridge、SideCar 返回外部调用方。

### 4.2 失败分类

| 阶段 | 错误码 | 行为 |
|------|--------|------|
| required 服务发现失败 | `REQUIRED_SERVICE_DISCOVERY_FAILED` | SideCar 不调用 Bridge，返回 503 |
| optional 服务发现失败 | `OPTIONAL_SERVICE_UNAVAILABLE` | 写入上下文，继续转发 |
| 内部鉴权失败 | `UNTRUSTED_INTERNAL_REQUEST` | Bridge 返回 401/403，不进入 AgentFlow |
| Schema 不支持或字段非法 | `INVALID_PLATFORM_CONTEXT` | Bridge 返回 400，不进入 AgentFlow |
| Bridge 不可达 | `BRIDGE_UNREACHABLE` | SideCar 返回 502 |
| Node 连接快照中的实例失败 | `RESOLVED_SERVICE_UNREACHABLE` | 本次执行失败；不在执行中回调 SideCar 刷新 |

## 5. 对外兼容端点

| 端点 | 处理方式 | 说明 |
|------|----------|------|
| `GET /health`、`GET /healthcheck` | SideCar 本地 | 健康检查别名 |
| `GET /sysagent/v1/scheduleTaskAbility` | SideCar 本地 | 能力宣告 |
| `POST /coremind/systemagent/v1/callback` | SideCar → Bridge | 主任务入口 |
| `POST /health/v1/tasks` | SideCar → Bridge | 兼容别名 |
| `POST /aisf/outfit/v1/tasks` | SideCar → Bridge | 兼容别名 |
| `POST /aisf/sysagent/v1/agentInstall` | SideCar 本地 Stub | 返回 200，不执行实际安装 |
| `POST /aisf/sysagent/v1/agentUpdate` | SideCar 本地 Stub | 返回 200，不执行实际更新 |
| `POST /aisf/sysagent/v1/agentUninstall` | SideCar 本地 Stub | 返回 200，不执行实际卸载 |

`/resolve/*` 不属于 Bridge 的运行时依赖接口。若为诊断保留，只能放在受控管理面，
不得成为业务主链路或 Node 的服务发现入口。

## 6. 开发落点

```text
frontend/bridge/
├── coremind_bridge/
│   ├── main.cpp                 # 外部入口、发现编排、内部 envelope 转发
│   ├── resolve_handler.cpp      # CoreMind 服务发现
│   └── ...
├── routes/
│   └── coremind_internal.py     # 目标：内部回调入口与可信上下文校验
└── tests/
    └── test_coremind_sidecar_e2e.py
```

## 7. E2E 验收边界

E2E 必须启动真实 C++ SideCar、Python Bridge、AgentFlow daemon 和确定性 Node，验证：

- 外部请求只能按 `SideCar → Bridge → AgentFlow → Node` 流转；
- Node 收到的服务目录来自 SideCar 生成的请求级上下文；
- 外部伪造 `platform_context` 不会进入执行上下文；
- required 发现失败时 Bridge 和 Node 均不被调用；
- Node 连接失败时本次请求明确失败，不发生 Bridge → SideCar 回调；
- 不通过 Adapter Stub 或仅检查 SideCar 转发成功来替代完整链路。

## 8. 待确认规格

- 内部端点采用同机 loopback、Unix Domain Socket，还是容器内部网络；
- 每个外部入口对应的 required/optional 服务清单；
- 服务实例选择策略由 SideCar 固定首实例，还是将候选列表交给 Node。

以上事项不影响“不使用 TTL、不反向调用 SideCar、上下文仅请求级有效”的核心决策。
