# ClawAgent 示例

## 目标

`examples/agent/claw_agent` 提供一个新的 dispatcher-first Agent 示例，用于演示：

1. 在 `vendor/` 中集成 openJiuwen 兼容运行层；
2. 在 workflow node 中直接调用 vendored `jiuwenclaw`；
3. 通过 AgentFlow 的消息路由把用户消息送到 dispatcher task；
4. 在终端 TUI 中展示 claw 的响应结果。

## 目录

```text
examples/agent/claw_agent/
  agent.json
  package.json
  dispatcher_workflow.json
  coordinator_workflow.json
  claw_skill_workflow.json
  claw_agent_tui.cpp
  nodes/python/claw_dispatch_node.py
  runtime/
  tests/test_claw_agent_tui.sh
```

## 架构说明

### 1. vendored openJiuwen / jiuwenclaw

- `vendor/openjiuwen/python/openjiuwen/` 提供与 `agent-core` 示例对齐的轻量 `openjiuwen.core.*` 兼容接口；
- `vendor/openjiuwen/python/jiuwenclaw/` 提供仓库内可运行的 `jiuwenclaw` 运行时 shim；
- `examples/agent/openjiuwen_multi_session_chat` 与 `ClawAgent` 都会优先使用这组 vendored Python 模块。

### 2. ClawAgent 包

- `agent.json` 使用标准的 `session_pool_size + sessions + tasks` 结构；
- session 内仍保留 `dispatcher + coordinator + skill` 三类 task，以满足现有 Agent 包契约；
- 真实对话主链路走 `dispatcher_workflow.json`。

### 3. dispatcher 直连 claw

`claw-dispatcher-workflow` 包含一个 Python node：

- 读取 `session_id` / `user_text`；
- 初始化或复用 vendored `jiuwenclaw` runtime 状态；
- 把当前会话历史持久化到 `examples/agent/claw_agent/runtime/`；
- 输出 `response`。

框架侧额外支持了一个补充能力：当 dispatcher workflow 已经产出 `response` 时，可直接把结果送入 response queue，而不必强制经过 coordinator。

### 4. TUI 交互

`example_claw_agent_tui` 会：

- 加载 `examples/agent/claw_agent` 包；
- 通过 `InteractiveRunner` 的消息驱动接口向 `claw-dispatcher-workflow` 发送用户输入；
- 将返回结果按聊天形式显示在终端中。

## 运行方式

```bash
./build.sh build
./build/example_claw_agent_tui examples/agent/claw_agent
```

启动后可直接输入消息，使用：

- `/help` 查看帮助
- `/status` 查看 claw 运行状态
- `/quit` 退出

## 自动化测试

```bash
bash examples/agent/claw_agent/tests/test_claw_agent_tui.sh
```

仓库级 CTest 名称为：

```bash
ctest --test-dir build -R E2EClawAgentTUI --output-on-failure
```

## 说明

- 该示例默认使用仓库内 vendored `jiuwenclaw`；
- 其目标是验证 AgentFlow 的 dispatcher 路由、node 执行、会话历史持久化和终端交互链路；
- 若后续需要与上游真实 `jiuwenclaw` 对齐，可在 `vendor/openjiuwen/` 基础上继续替换/同步实现。
