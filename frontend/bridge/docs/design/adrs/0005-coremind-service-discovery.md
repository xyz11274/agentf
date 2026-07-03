# ADR-0005：由 C++ SideCar 单向传递 CoreMind 服务发现上下文

> **层级**：L3 · `docs/design/adrs/0005-coremind-service-discovery.md`
> **状态**：Accepted
> **更新日期**：2026-07-03
> **详细设计**：[CoreMind Bridge](../architecture/modules/coremind-bridge.md)

## Context

Python Bridge 无法直接调用 CoreMind C++ 服务发现能力。C++ SideCar 同时是生产环境唯一外部入口，
业务主链路必须保持：

```text
用户 → C++ SideCar → Python Bridge → AgentFlow → Node
```

若 Bridge 在 SideCar 转发请求期间同步反向调用 `/resolve`，会形成运行时依赖环：SideCar 等待
Bridge，Bridge 又等待 SideCar。该结构会放大线程池耗尽、超时和故障传播风险。

服务发现结果不存在可信的刷新通道，因此不能用臆测 TTL 表达“结果可刷新”或“过期后可恢复”。

## Decision

SideCar 在转发每个外部请求前完成该入口所需的白名单服务发现，并将结果放入经过鉴权的内部
envelope，单向传递给 Bridge。Bridge、AgentFlow 和 Node 不反向调用 SideCar，也不跨请求缓存
服务发现结果。

服务快照仅对本次请求有效：

- 不包含 TTL；
- 不提供执行中的刷新；
- 后续新请求重新发现；
- 当前请求连接失败时明确失败。

SideCar 通过专用内部端点把 envelope 发送给 Bridge。内部端点必须同时受网络边界和共享凭据
保护；外部载荷中的同名上下文字段不能覆盖 SideCar 生成的内容。

## Alternatives

| 方案 | 结论 | 原因 |
|------|------|------|
| Bridge 同步调用 SideCar `/resolve` | 拒绝 | 形成运行时依赖环 |
| SideCar 随请求传递服务快照 | 采用 | 单向、请求级、与现有执行上下文自然衔接 |
| 为快照设置 TTL | 拒绝 | 当前没有刷新途径，TTL 无法形成闭环 |
| SideCar 启动时一次性推送 | 拒绝 | 启动顺序耦合，且后续变化无法可靠同步 |
| Python 直连注册中心 | 拒绝 | 无法复用 CoreMind C++ 认证与发现能力 |
| pybind11 链接 CoreMind | 拒绝 | Python ABI 与平台库版本耦合 |

## Consequences

### Positive

- 业务调用链没有 Bridge → SideCar 的反向依赖；
- 每次请求使用当次发现结果，不承诺无法实现的刷新语义；
- 平台上下文来源可鉴别，用户不能指定任意服务或伪造发现结果；
- Node 通过统一执行上下文消费服务目录，不感知 CoreMind C++ API。

### Negative

- 每个请求都会产生服务发现开销；
- 长任务执行期间服务实例变化不会自动刷新；
- SideCar 必须知道不同入口所需的 required/optional 服务集合；
- 需要新增 Bridge 内部入口、鉴权和上下文映射。

### Failure semantics

- required 服务发现失败：SideCar 返回 503，不调用 Bridge；
- optional 服务发现失败：写入结构化不可用状态并继续；
- Bridge 内部鉴权或 Schema 校验失败：拒绝请求，不进入 AgentFlow；
- Node 无法连接已解析实例：本次执行失败，不回调 SideCar 刷新。

SysAgent/AccessCtrl 兼容 aisf_adapter 属于 Python Bridge 的协议边界，代码只能放在
`frontend/bridge/aisf_adapter/`。当前候选实现不注册路由、不独立启动，也不参与生产调用链；
待契约与鉴权评审后再决定是否接入。
