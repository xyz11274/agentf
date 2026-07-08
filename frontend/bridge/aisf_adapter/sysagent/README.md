# SysAgent 适配层候选实现（尚未接入）

> 代码归属：Python Bridge 基础设施。当前实现仅迁移保存，不注册 FastAPI 路由、
> 不加入 `scripts` 启动链，也不作为 `outfit-assistant` 的组成部分。

对标 HealthAgent 的 TaskRouter + AgentFlowManager + HaContext 机制的 Python 实现。

## 解决的问题

outfit-assistant 运行在 `agentflowd` (JSON-RPC over TCP) 之上。  
上游 SysAgent 使用 `AccessCtrl` 消息格式（HTTP，PascalCase 字段名）下发任务。

适配层 = 翻译官 + 上下文管理器 + 异步回调器：

```
上游 SysAgent
  │ POST /coremind/systemagent/v1/callback
  │ Body: {Header:{...,TaskID:"id",SysAgentEntity:"10.0.0.1"},
  │        Data:{MessageType:"TaskAssignment",UserInputText:"北京天气"}}
  ▼
┌──────────────────────────────────────────────────────┐
│  Bridge AISF 适配层 (aisf_adapter/sysagent/server.py)      │
│                                                      │
│  ① parse_accessctrl() → PascalCase 字段解析          │
│     → Header.TaskID, Data.UserInputText 等           │
│                                                      │
│  ② HaContext 管理                                    │
│     → 存储上游地址(sysAgentEntity)、taskID、用户信息  │
│                                                      │
│  ③ 消息路由 (↔ HealthAgent HandleAgentMsgRequest)    │
│     TASKASSIGNMENT:                                   │
│       receiver=GUIAgent → 桥接转发(占位)              │
│       receiver=HealthAgent → 执行 Agent + 回传结果    │
│     非 TASKASSIGNMENT:                                │
│       sender=GUIAgent → GUI 回调(用已有 did/sid 重入) │
│       其他 → 转发结果回上游                            │
│                                                      │
│  ④ agent.execute + poll_response                     │
│                                                      │
│  ⑤ 回传结果:                                         │
│     → TaskStatusReport (code=0 完成)                  │
│     → UserOutput (最终文本)                           │
│     → HTTP POST 回上游: http://{sysAgentEntity}:23767 │
│                       /coremind/systemagent/v1/callback│
└──────────────────────────────────────────────────────┘
  │ JSON-RPC 2.0 over TCP
  ▼
agentflowd → outfit-assistant workflow
```

## 工作模式

| 模式 | 设置 | 行为 | 对标 HealthAgent |
|------|------|------|-----------------|
| **async** (默认) | `SYNC_MODE=0` | 收到请求立即返回 200，后台执行完成后 POST 回上游回调地址 | ✅ 完全一致 |
| **sync** | `SYNC_MODE=1` | 收到请求后等执行完成再返回 HTTP 响应 | 仅用于测试 |

## 架构对应关系

| HealthAgent 组件 | 适配层对应 | 匹配状态 |
|-----------------|-----------|---------|
| `CMFrm::Starter::InitFramework` (HTTP/2 服务) | `uvicorn.run(app)` (HTTP/1.1) | ⚠️ 协议不同 |
| `TaskRouter::Register` (路由注册) | ASGI `path` 匹配 | ✅ |
| `HandleUserRequest` (请求入口) | `handle_callback()` | ✅ |
| `HandleAgentMsgRequest` (消息路由) | `handle_callback()` 内的 if/else 分支 | ✅ |
| `AccessCtrl` 解析 (PascalCase) | `parse_accessctrl()` | ✅ |
| `HaContext` 存储/更新 | `HaContextStore` | ✅ |
| `updateHaContextInternalIds` | `ha_store.update_dialogue_ids()` | ✅ |
| `AgentFlowManager::SendMsg` | `daemon.call("agent.execute", ...)` | ✅ |
| `StartListeningDataBus` (轮询) | `agent.poll_response` 循环 | ✅ |
| `BuildUserOutputMsg` | `build_user_output()` | ✅ |
| `BuildTaskStatusReportMsg` | `build_task_status_report()` | ✅ |
| `ForwardToExternalSysAgent` | `post_result_to_upstream()` | ✅ |
| `HttpPostToSysAgent` (回传) | `http_post_to_upstream()` | ✅ |
| GUI 桥接转发 | `receiver == "GUIAgent"` 分支 (占位) | ⬜ 未实现 |
| `HandleScheduleTaskAbility` | `/sysagent/v1/scheduleTaskAbility` | ✅ |
| Agent 安装/更新/卸载 | `/aisf/sysagent/v1/agent*` | ✅ (桩) |
| 服务发现注册 | 无 (需外部注册中心) | ❌ 不支持 |

## 用法

### 1. 启动 agentflowd

```bash
cd /path/to/aisf_agentflow
set -a; . examples/agent/outfit-assistant/.env; set +a
PYTHON_PATH="$(pwd)/examples/agent/outfit-assistant/.venv" \
  ./build/agentflowd \
  --listen 0.0.0.0:17891 \
  --package "$(pwd)/examples/agent/outfit-assistant"
```

### 2. 历史独立启动方式（已停用）

```bash
cd /path/to/aisf_agentflow
pip install httpx uvicorn

# async 模式 (默认)
python -m frontend.bridge.adapters.sysagent.server  # 当前不支持，仅保留历史说明

# sync 模式 (调试用)
SYNC_MODE=1 python -m frontend.bridge.adapters.sysagent.server  # 当前不支持
```

### 3. 上游 SysAgent 发送任务

**注意：字段必须是 PascalCase（`Header`、`Data`、`UserInputText`），这是上游 SysAgent 的实际格式。**

```bash
curl -X POST http://localhost:18080/coremind/systemagent/v1/callback \
  -H 'Content-Type: application/json' \
  -d '{
    "Header": {
      "MessageType": "UserTaskManagement",
      "TaskID": "ha_001",
      "Sender": "SystemAgent",
      "Receiver": "HealthAgent",
      "UserName": "user_001",
      "DeviceId": "dev_001",
      "SysAgentEntity": "10.0.0.1"
    },
    "Data": {
      "MessageType": "TaskAssignment",
      "UserInputText": "今天北京天气如何，适合穿什么",
      "UserIntent": "health_consult"
    }
  }'
```

async 模式返回（立即）：

```json
{"status": "accepted", "task_id": "ha_001", "mode": "async"}
```

随后适配层会 POST 回上游 `10.0.0.1:23767`：

```json
{"Header":{...,"MessageType":"TaskStatusManagement"},"Data":{"TaskStatusCode":0,...}}
{"Header":{...,"MessageType":"DataExchange"},"Data":{"MessageType":"UserOutput","UserOutputText":"今天北京 29°C..."}}
```

sync 模式返回（等待执行完成）：

```json
{"status": "completed", "task_id": "ha_001", "content": "今天北京 29°C..."}
```

## 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `AISF_ADAPTER_HOST` | `0.0.0.0` | 适配层监听地址 |
| `AISF_ADAPTER_PORT` | `18080` | 适配层监听端口 |
| `AISF_ADAPTER_SCHEME` | `http` | 能力宣告中的 URL scheme |
| `DAEMON_HOST` | `127.0.0.1` | agentflowd 地址 |
| `DAEMON_PORT` | `17891` | agentflowd 端口 |
| `AGENT_ID` | `outfit-assistant` | AgentFlow 中的 agent 名称 |
| `CALLBACK_TIMEOUT` | `360.0` | 单次任务超时（秒） |
| `SYNC_MODE` | `0` | `1`=同步, `0`=异步 |
| `UPSTREAM_CALLBACK_PORT` | `23767` | 上游 SysAgent 回调端口 |
| `UPSTREAM_CALLBACK_PATH` | `/coremind/systemagent/v1/callback` | 上游回调路径 |

## 对接内部模型服务（IMInferScheduler）

outfit-assistant 的节点通过 `AF_LLM_BASE_URL` 环境变量决定调哪个模型服务。
默认调外部 OpenAI API，也可以改为调内部模型调度服务 `IMInferScheduler`。

### IMInferScheduler 是什么

HealthAgent 内部使用的模型调度服务。节点（Rust/Python）不直接连 LLM，
而是通过这个服务统一调度，地址通过服务发现获得。

```
coordinator_decide.py / skill_executor.py (节点)
  │ 读 AF_LLM_BASE_URL
  │
  ├─ 方式 A: https://api.openai.com/v1           ← 外部 OpenAI
  │
  └─ 方式 B: http://{im_infer_ip}:27321/infermind/llm/v1  ← 内部模型
                  ↓
            IMInferScheduler
                  ↓
            模型实例 (Qwen3-32B 等)
```

### 调用方式

IMInferScheduler 的接口是：

| 属性 | 值 |
|------|-----|
| 协议 | **HTTP**（非 HTTPS） |
| 端口 | `27321`（源码硬编码） |
| 路径 | `/infermind/llm/v1/chat/completions` |
| 格式 | OpenAI 兼容（messages / SSE 流式） |
| 认证 | **不需要**（纯内网，无 Authorization header） |
| 额外 header | `X-Task-Type: SysAgentIntent`（路由标记） |

### 配置方法

```bash
# .env 或启动前 export

# 方式 A：调外部 OpenAI（当前默认）
AF_LLM_BASE_URL=https://api.openai.com/v1
API_KEY=sk-xxx

# 方式 B：调内部 IMInferScheduler（HTTP，无证书）
AF_LLM_BASE_URL=http://10.0.0.5:27321/infermind/llm/v1
# 不需要设置 API_KEY（内部服务无认证）

# 可选：通过 SOCKS5 代理访问（HealthAgent 通过 MMGateWay 代理）
HTTP_PROXY=socks5://{mmgateway_ip}:27262
```

### 如何获得 IMInferScheduler 的 IP

IMInferScheduler 是独立微服务，HealthAgent 通过 CoreMind 服务发现自动获取地址。
你没有服务发现时，用以下方式找到它：

```bash
# 1. 问运维 / 看部署文档
# 2. 如果和 HealthAgent 在同一台机器
netstat -an | grep 27321
# 3. 看 HealthAgent 日志（已启动的服务发现缓存）
grep "IMInferScheduler" /opt/coremind/log/*.log
grep "Endpoint is" /opt/coremind/log/*.log
# 4. 查配置文件
grep -r "27321" /opt/coremind/conf/
```

节点代码不需要改，`AF_LLM_BASE_URL` 一换就行。

## 暴露的端点

| 路径 | 方法 | 用途 | 对标 HealthAgent |
|------|------|------|-----------------|
| `/healthcheck` | GET | 健康检查 | `HandleHealthCheck` |
| `/sysagent/v1/scheduleTaskAbility` | GET/POST | 能力宣告 | `HandleScheduleTaskAbility` |
| `/coremind/systemagent/v1/callback` | POST | **主任务入口** | `HandleUserRequest` |
| `/health/v1/tasks` | POST | 别名 | 兼容上游路径 |
| `/aisf/outfit/v1/tasks` | POST | 别名 | 简化路径 |
| `/aisf/sysagent/v1/agentInstall` | POST | Agent 安装 (桩) | `HandleAgentInstallRequest` |
| `/aisf/sysagent/v1/agentUpdate` | POST | Agent 更新 (桩) | `HandleAgentUpdateRequest` |
| `/aisf/sysagent/v1/agentUninstall` | POST | Agent 卸载 (桩) | `HandleAgentUninstallRequest` |

## 已知限制

- **HTTP/1.1 非 HTTP/2** — 上游若只支持 HTTP/2 需要额外适配层
- **GUI 桥接未实现** — `receiver=GUIAgent` 的消息会被跳过
- **无服务发现注册** — 需要手动配置上游的适配层地址
- **内存 HaContext** — 重启后丢失（可扩展为持久化存储）

## 文件结构

```
adapter/
├── server.py     # 适配层主程序 (818 行, 单文件)
└── README.md     # 本文档
```
