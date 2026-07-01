# AgentFlow 快速演示

> 演示 coordinator-only agent loop（方案 A：dispatcher 空 workflow → coordinator 内循环）
> 对应示例：`examples/agent/harness_agent/`

---

## 1. 编译

```bash
cd /mnt/d/coding/my_claw/aisf/AgentFlow

# 完整编译（首次需要，约 2-3 分钟）
./ar_scripts/build.sh

# 或只编译 agent_runner（增量，约 10 秒）
./ar_scripts/build.sh agent_runner
```

## 2. 配置 LLM（可选）

`run_agent.sh` 会自动加载 `ar_scripts/.env` 文件。如果不配，使用内置 mock LLM。

```bash
cd /mnt/d/coding/my_claw/aisf/AgentFlow

# 从模板创建 .env
cp ar_scripts/.env.example ar_scripts/.env

# 编辑 .env，填入真实的 LLM 配置
vim ar_scripts/.env
```

`.env` 内容示例：

```bash
AF_LLM_BASE_URL="https://api.openai.com/v1"
AF_LLM_API_KEY="sk-your-api-key-here"
AF_LLM_MODEL="gpt-4o-mini"
```

也可以直接 export 环境变量（优先级高于 .env）：

```bash
export AF_LLM_BASE_URL="https://api.openai.com/v1"
export AF_LLM_API_KEY="sk-..."
export AF_LLM_MODEL="gpt-4o-mini"
```

## 3. 验证包结构

```bash
./ar_scripts/run_agent.sh validate
```

预期输出：能看到 `Package: harness-agent v1.0.0` + 4 个 workflow 文件列表。

## 4. 列出 workflow

```bash
./ar_scripts/run_agent.sh list
```

预期输出：
```
  [dispatcher-pass-through] dispatcher-pass-through (2 nodes)
  [coordinator-loop-workflow] coordinator-loop-workflow (1 nodes)
  [echo-workflow] echo-workflow (1 nodes)
  [calculator-workflow] calculator-workflow (1 nodes)
```

## 5. 交互模式 — 测试 agent 循环

```bash
./ar_scripts/run_agent.sh
```

进入交互界面后，输入：

```
execute coordinator-loop-workflow --input "你好"
```

### 预期行为

`harness_agent` 的 `llm_think_and_act.py` 内置了 mock LLM，模拟两轮循环：

| 轮次 | LLM 决策 | 动作 |
|:----:|----------|------|
| 1 | 调用 echo 工具 | 写入 `next_skills=["echo"]` 到 DataBus，拉起 echo skill |
| 2 | 直接回复 | 输出 `response` |

所以你会看到类似：
```
Executing workflow: coordinator-loop-workflow
--- Output ---
Tool executed successfully. Result: 1 tool(s) completed.
```

### 如果配了真实 LLM

设置环境变量后启动即可调用真实 LLM：

```bash
export AF_LLM_BASE_URL="https://api.openai.com/v1"
export AF_LLM_API_KEY="sk-..."
export AF_LLM_MODEL="gpt-4o-mini"
./ar_scripts/run_agent.sh
```

## 6. 消息流（理解发生了什么）

```
你输入 "你好"
    │
    ▼
agent_runner → route_request_message()
    │
    ▼
SessionRouter → dispatcher task
    │  执行 dispatcher_pass_through.json（空 workflow）
    │  完成后 → 自动转发到 coordinator
    ▼
coordinator task
    │  ┌─────────────────────────────────────────────┐
    │  │ llm_think_and_act.py                        │
    │  │  → 调用 LLM（或 mock）                       │
    │  │  → LLM 决定调用 echo 工具                    │
    │  │  → 写入 next_skills=["echo"] 到 DataBus      │
    │  └─────────────────────────────────────────────┘
    │
    ▼
coordinator workflow 完成
    │  process_next_skills_launch() 读取 next_skills
    │  拉起 echo skill
    ▼
echo skill 执行 → workflow 完成
    │  通知 coordinator
    ▼
coordinator 自触发（agent.cpp:1534-1563）
    │  enqueue_intent_request_message(trigger)
    │  → coordinator workflow 再次执行
    ▼
LLM 评估结果 → 决定回复 → 输出 response
    │
    ▼
你在终端看到回复
```

## 7. 文件参考

| 文件 | 说明 |
|------|------|
| `ar_scripts/build.sh` | 编译脚本 |
| `ar_scripts/run_agent.sh` | 启动脚本 |
| `examples/agent/harness_agent/agent.json` | Agent 定义（方案 A） |
| `examples/agent/harness_agent/dispatcher_pass_through.json` | dispatcher 空 workflow |
| `examples/agent/harness_agent/coordinator_workflow.json` | coordinator LLM 循环 workflow |
| `examples/agent/harness_agent/nodes/llm_think_and_act.py` | LLM 思考节点（含 mock） |
| `examples/agent/harness_agent/nodes/tool_echo.py` | echo 工具 |
| `examples/agent/harness_agent/nodes/tool_calculator.py` | 计算器工具 |
| `docs/usage/plan/coordinator-only-agent-loop-实现计划.md` | 完整实现计划 |
