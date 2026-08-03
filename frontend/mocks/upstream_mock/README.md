# CoreMind 上游 Mock 服务器

用于完整模拟 Sidecar 异步回调链路的测试工具。

## 架构

```
前端 (Sidecar 模拟页面)
  │ POST /upstream-mock/send
  │ GET  /upstream-mock/events/{task_id} (SSE)
  ▼
上游 Mock 服务器 (:23768)
  │ POST /coremind/systemagent/v1/callback (带 SysAgentEntity=自身)
  │ Header.MessageType=UserTaskManagement / Data.MessageType=TaskAssignment
  │ UserInputObject 为 stringified JSON string（对齐 coreMind-mode）
  ▼
Sidecar C++ (:23767)
  │ 200 accepted（coreMind-mode 异步路径只返回 200 无 body）
  │ 后台 SSE → Bridge → daemon
  │ 后台 POST UserOutput/TaskStatus → Mock 回调端点
  ▼
上游 Mock ← 接收回调 → SSE 推送前端
```

> 协议对齐说明（对齐 sidecar coreMind-mode 实际行为）：
> - **入站消息类型**：`Header.MessageType=UserTaskManagement` + `Data.MessageType=TaskAssignment`
>   （或 `DataExchange` + `UserInput`），否则 sidecar 返回 400 unsupported message type。
> - **UserInputObject**：CMF 协议要求 stringified JSON string，传嵌套对象会被 sidecar 丢弃多模态输入。
> - **异步响应**：coreMind-mode 只返回 200 无 body（对齐 SysAgent）；httplib dev 模式返回
>   `{"status":"accepted","request_id":"sidecar-N"}` 且回调 Header.TaskID=request_id。
> - **回调 Header**：coreMind-mode 回调 Header.TaskID=原始 task_id、Sender/Receiver 翻转、
>   透传 AcmSessionId/ClientExField/DeviceCategory/DeviceId/SessionId/TeamEnvironmentID/UUID。

## 启动

```bash
# 确保 sidecar + Bridge + daemon 已运行

# 启动 Mock 服务器
bash scripts/start_upstream_mock.sh
# 或直接:
python3 frontend/mocks/upstream_mock/server.py
```

## 端点

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/send` | 前端触发请求，构造 AccessCtrl POST 到 sidecar |
| POST | `/coremind/systemagent/v1/callback` | sidecar 回调入口 |
| GET | `/events/{task_id}` | SSE 流式回调事件 |
| GET | `/tasks/{task_id}` | 查询 task 状态和已收到的回调 |
| GET | `/health` | 健康检查 |
