# multi_session_chat

多轮对话 Agent 示例统一目录。

## 目录结构
- `server/`：独立 C++ 进程（HTTP + WebSocket + 对话管理）
- `package/`：Agent 包资产（agent.json/package.json/workflows/prompts/scripts）
- `scripts/`：构建与运行脚本
- `tests/`：自动化对话测试脚本

## 快速运行
```bash
bash examples/multi_session_chat/scripts/build_and_run.sh
```

## 自动化测试
```bash
bash examples/multi_session_chat/tests/auto_dialog_test.sh
```
