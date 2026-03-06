# multi_session_chat

多轮对话 Agent 示例，展示 AgentFlow 的**多语言节点**、**DataBus 驱动**和**跨 session 协调编排**能力。

## 目录结构
```
multi_session_chat/
  server/        # 独立 C++ 进程（HTTP + WebSocket + 对话管理）
  package/       # Agent 包（agent.json, workflows, prompts, scripts）
  nodes/
    cpp/         # C++ SO 库节点（dispatcher/coordinator/session-dispatcher）
    go/          # Go 节点（session-context-workflow）
    python/      # Python 节点（skill-date-workflow）
    java/        # Java 节点（skill-delegate-workflow）
  scripts/       # 构建与运行脚本
  tests/         # 自动化对话测试
```

## 多语言节点实现

| Workflow | 节点 | 语言 |
|---|---|---|
| `default-dispatcher-workflow` | `pipeline` | **C++** SO 库 |
| `default-coordinator-workflow` | `collect`, `plan`, `apply` | **C++** SO 库 |
| `session-dispatcher-workflow` | `load_context`, `select_skill`, `create_task` | **C++** SO 库 |
| `session-context-workflow` | `read_prompt`, `read_history`, `summarize`, `fill_context` | **Go** |
| `skill-date-workflow` | `date-calc` | **Python** |
| `skill-delegate-workflow` | `agent-delegate` | **Java** |

## 快速运行

### 前置依赖
- C++ 工具链（CMake + clang/gcc）
- Go 1.18+（session-context-workflow 运行时需要）
- Java 11+（可选，用于 skill-delegate-workflow）
- Python 3.8+（skill-date-workflow 运行时需要）

### 一键构建启动
```bash
bash examples/multi_session_chat/scripts/build_and_run.sh
```

打开 `http://127.0.0.1:8088/`，页面显示多轮对话（支持历史回放）。

## LLM 配置
```bash
export LLM_BASE_URL="https://api.openai.com/v1"
export LLM_API_KEY="sk-..."
export LLM_MODEL="gpt-4o-mini"
```
未设置时返回 mock 文本，但完整的多语言节点流程仍正常执行。

## 自动化测试
```bash
bash examples/multi_session_chat/tests/auto_dialog_test.sh
```

## 详细文档
见 [docs/multi-session-chat-example.md](../../docs/multi-session-chat-example.md)

