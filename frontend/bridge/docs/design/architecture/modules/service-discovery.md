# CoreMind 服务发现上下文设计

> **层级**：L4 · `architecture/modules/service-discovery.md`
> **状态**：部分实现；SysAgent adapter 尚未接入
> **更新日期**：2026-07-03
> **架构决策**：[ADR-0005](../../adrs/0005-coremind-service-discovery.md)

## 1. 范围与当前状态

本文档定义 C++ SideCar、Python Bridge 与候选 SysAgent adapter 的职责边界。

| 能力 | 当前状态 | 权威落点 |
|------|----------|----------|
| CoreMind 服务发现 | 已实现 | C++ SideCar |
| 请求级可信服务快照 | 已实现 | SideCar → Bridge 内部 envelope |
| Bridge 校验并转发可信上下文 | 已实现 | `routes/coremind_internal.py` |
| SideCar/aisf_adapter 配置模型 | 已实现并保留 | `config.py`、`config.yaml` |
| SysAgent AccessCtrl 协议适配 | 候选代码，未接入 | `frontend/bridge/aisf_adapter/sysagent/` |
| Python 直连注册中心 | 不支持 | NA |
| Bridge 反向调用 SideCar `/resolve` | 禁止 | ADR-0005 |

`examples/agent/*` 只承载业务 Agent package，不承载平台协议 adapter。

## 2. 逻辑架构

```text
用户请求
   ↓
C++ SideCar
   ├─ 调用 CoreMind 服务发现
   ├─ 构造请求级 trusted_context
   └─ 携带内部凭据转发
   ↓
Python Bridge 内部入口
   ├─ 校验凭据与 Schema
   ├─ 拒绝外部同名字段覆盖
   └─ 将上下文提交 AgentFlow
   ↓
agentflowd → Node
```

依赖方向保持单向。Bridge、AgentFlow、Node 不反向调用 SideCar，也不缓存跨请求服务发现结果。

## 3. 请求级服务快照

- SideCar 在每次外部请求进入时重新发现该入口白名单中的服务。
- 快照不设置 TTL；当前没有执行中刷新通道，TTL 过期无法形成恢复闭环。
- required 服务发现失败时，SideCar 返回 503，不调用 Bridge。
- optional 服务不可用时，SideCar 传递结构化不可用状态，由下游按契约处理。
- Node 连接快照中的实例失败时，本次任务明确失败，不回调 SideCar 刷新。

## 4. SysAgent adapter 归属与候选集成方式

候选实现已从 `examples/agent/outfit_assistant/adapter` 迁至：

```text
frontend/bridge/aisf_adapter/sysagent/
├── __init__.py
├── server.py
└── README.md
```

它属于 Bridge 的入站/出站协议适配层，而不是任何具体 Agent package。

### 4.1 目标集成方式

若后续确认仍需 AccessCtrl 兼容，采用“Bridge 内部 router + application service”方式：

1. `aisf_adapter/sysagent/router.py` 只负责 AccessCtrl HTTP 契约与鉴权；
2. `aisf_adapter/sysagent/mapper.py` 负责 AccessCtrl 与 Bridge 内部请求/响应模型转换；
3. 复用 Bridge 已有 daemon client、session 路由、artifact URL 处理及生命周期；
4. 不再独立启动第二个 Uvicorn，不直接自行连接 agentflowd；
5. 服务发现只消费 SideCar 注入的可信上下文，不实现 Python 注册中心客户端。

### 4.2 当前明确不接入

- 不在 `main.py` 注册 adapter router；
- 不加入 `ar_scripts/start_bridge.sh` 或生产启动链；
- 不启用 aisf_adapter 独立监听端口；
- 不承诺候选实现当前的数据模型与回调语义可直接用于生产。

现有 `sidecar.*`、`aisf_adapter.*` 配置字段属于已经完成的配置能力，必须保留。它们为后续接入提供
部署参数，但"字段存在"不等于候选 router 已经进入当前运行链；接入时需要明确这些字段如何
映射到 Bridge 内部模块，并处理 `sidecar.aisf_adapter_url` 的兼容命名。

接入前必须完成 AccessCtrl 契约、鉴权边界、异步回调幂等性及错误模型评审。

## 5. 被拒绝方案

| 方案 | 结论 | 原因 |
|------|------|------|
| Adapter 放在示例 Agent 中 | 拒绝 | 平台协议职责污染业务 package，无法被其他 Agent 复用 |
| Adapter 独立进程直接连接 daemon | 暂不采用 | 重复 Bridge 的连接、session、artifact 和生命周期能力 |
| Bridge 同步调用 SideCar `/resolve` | 拒绝 | 形成 SideCar → Bridge → SideCar 运行时依赖环 |
| Python 直连 CSE/Consul | 拒绝 | 绕过 CoreMind 认证与发现实现，产生双重真相来源 |

## 6. 验证边界

- SideCar → Bridge → agentflowd → Node 的可信上下文 E2E 已由
  `test_coremind_sidecar_e2e.py` 覆盖。
- SysAgent adapter 当前只保存候选代码，不应出现在启动进程、路由表或 E2E 主链中。
- 后续接入测试必须验证外部载荷不能覆盖 SideCar 生成的 trusted_context。
