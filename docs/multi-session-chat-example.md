# Multi-Session Chat Example (AgentFlow)

## 目录
- 根目录：`examples/multi_session_chat/`
- 进程入口：`examples/multi_session_chat/server/chat_server.cpp`
- WebUI：`examples/multi_session_chat/server/webui/index.html`
- Agent包：`examples/multi_session_chat/package/`
- 脚本：`examples/multi_session_chat/scripts/`
- 自动化测试：`examples/multi_session_chat/tests/`

## 功能映射
1. **独立 agent 进程 + WebUI + 对话管理落盘**  
   `example_chat_server` 直接加载 MMAgent 包并初始化 MMAgentRuntime，提供 HTTP + WebSocket，并将消息按 `session_id` 存到 `package/history/*.jsonl`；WebUI 通过 `/api/chat` 和 `/api/history` 展示多轮消息。

2. **多轮对话包 + precreate_session_count=4 + task0 dispatcher**
   - `agent.json` 设置 `precreate_session_count: 4`
   - 默认 session 使用 `default-dispatcher-workflow` 广播请求到其他 session
   - 非默认 session 使用 `session-dispatcher-workflow`：读取 context、选择 skill、生成未启动 skill task 计划

3. **session_context_workflow**
   `session-context-workflow` 节点链路：读取 prompt 模板 -> 读取历史 -> LLM 摘要 -> 填充 context。

4. **4个不同 skill**
   - `skill-search-workflow`
   - `skill-summary-workflow`
   - `skill-date-workflow`
   - `skill-delegate-workflow`

5. **默认 session 响应编排（真实回复）**
   `default-dispatcher-workflow` 串行执行：广播输入 -> 选择 skill -> 收集 skill -> 执行 skills -> 调用 LLM 汇总生成最终回复，最终回复经 runtime/databus 回传到 WebUI。

## LLM配置
采用 OpenAI Chat Completions 兼容接口，环境变量：
- `LLM_BASE_URL`
- `LLM_API_KEY`
- `LLM_MODEL`

默认无 `LLM_API_KEY` 时会返回 mock 文本，但仍走完整 agent+skills 流程。

## 运行
```bash
./build.sh build
./build/example_chat_server --package examples/multi_session_chat/package --web-root examples/multi_session_chat/server/webui
```

## 自动化对话测试
```bash
bash examples/multi_session_chat/tests/auto_dialog_test.sh
```
