# Runtime、Daemon 与生命周期模块改动

## 1. 模块职责

该模块处理一轮对话何时开始、等待、超时、取消、清理和真正结束。它跨越 Bridge、Daemon、C++ Agent、Runtime Worker 和 Sandbox 生命周期。

主文件：

- frontend/bridge/routes/chat_rest.py
- frontend/bridge/client.py
- frontend/bridge/services/session_sync.py
- src/daemon/daemon.cpp
- src/daemon/rpc_handler.cpp
- src/daemon/rpc_server.cpp
- src/daemon/response_poller.cpp
- src/agent/session.cpp
- src/runtime/runtime.cpp
- src/runtime/worker/exe_loader.cpp
- src/sandbox/sandbox_local.cpp

## 2. 总预算而不是层层叠加

当前超时形成自上而下的剩余预算：

- Bridge Turn 有总等待预算。
- Daemon execute 和 wait 使用该轮剩余时间。
- Skill 节点使用 node_deadline。
- 每轮 LLM 将 AF_LLM_TIMEOUT 压到节点剩余预算内。
- Tool Batch 和单个 Tool 也服从剩余预算。
- 预留少量时间生成结构化终态、关闭 Span 和写轨迹。

因此 LLM 重试或多个 Tool 不会各自重新获得完整节点超时，减少“外层已经超时，内层仍运行很久”的情况。

## 3. 超时和断开会真正取消

Bridge 不再只停止等待：

- execute 超时会按 agent、user、client、dialogue 的精确路由找到 Daemon Session。
- SSE 客户端断开也会发 Dialogue Interrupt。
- Daemon 清理响应队列和活动任务。
- Harness Batch 取消 active 与 queued 分支。
- Skill Executor 轮询 interrupt 标记并形成 interrupted 终态。

这样前端超时后不会留下继续调用 LLM、Tool 或网络的孤儿任务。

## 4. Session TTL 与活跃保护

内部 Session 可由 TTL 回收，但当前增加以下保护：

- 有活跃 Dialogue、Task 或 Harness Batch 时不回收。
- Sandbox 被复用时刷新活跃时间。
- Dialogue 完成或取消后才进入正常清理。
- 清理事件在内部状态真正清空后再通知订阅者。

## 5. Daemon 与 Sandbox 模式

sandbox-mode=none 仍会初始化 Daemon SessionManager。Sandbox 开关只决定 Tool 在哪里执行，不应让 Agent/Session RPC 整体不可用。

Local Sandbox 的实例端口和生命周期与 Session 路由绑定，避免多个会话共享错误 endpoint。

## 6. Bridge 进程管理

Bridge restart 改为跟踪真正的 Uvicorn worker，而不是只管理外层 shell：

- 启动后可以确认实际服务进程。
- restart/stop 能终止旧 worker，避免端口仍被占用。
- 日志脱敏 Filter 保留 Uvicorn 的格式化参数契约，不因安全处理触发 logging error。

## 7. 故障域隔离

当前区分：

- Provider/Tool 业务失败。
- Skill 业务失败。
- Workflow 执行失败。
- Batch 部分或全部失败。
- Bridge 传输失败。
- 轨迹写入失败。

前四类各自形成状态；Bridge 传输失败记录 delivery；轨迹失败只报警。一个故障域不应无条件把其他层改写成相同状态。

## 8. 形成的不变量

- 上层超时必须能向下取消，而不是只丢弃响应。
- 所有子操作共享父节点剩余预算。
- 活跃 Dialogue 不能被 TTL 或 GC 回收。
- sandbox-mode=none 不能破坏 Daemon 会话管理。
- 结束通知只能在真实清理和真实终态之后发出。

对应变更主要见 CHG-017、021、030、031、039、049、060、061、084、085、088、089、090、091、102、107、114、116。

