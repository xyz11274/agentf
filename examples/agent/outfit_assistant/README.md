# Outfit Assistant

穿搭助手 — 基于 AgentFlow 框架的 coordinator-driven agent，结合天气、场合和用户画像提供穿搭建议。

## 架构总览 (V3)

```
用户请求
  ↓
Dispatcher (next_skills=["dummy_pre_skill"])
  ↓
dummy_pre_skill [type=skill, hook 点] ── dummy_pre_skill.py
  │   纯 passthrough，不处理不输出 next_skills
  │   完成后 framework 自动 forward 给 coordinator task
  ↓ (framework forward，从 {dialogue_id}/user_prompt 恢复原始输入)
coordinator [type=coordinator] ── coordinator_decide.py
  │   单轮 LLM 决策：调 run_skills 工具 or 输出最终回复
  │
  ├─ 若 LLM 调 run_skills → 写 dispatch manifest 到 DataBus
  │     ↓ (coordinator 输出 next_skills，framework launch)
  │   skill-runner [type=skill] ── skill_executor.py
  │     读 exec_skill_key → DataBus dispatch manifest → 真实 skill_name + prompt
  │     从 harness/skills/{skill_name}/scripts/ 加载 skill 自带 tool
  │     LLM while-loop 执行任务
  │     ↓
  │   skill 完成 → framework 自动重触发 coordinator (从 user_prompt 恢复输入)
  │
  └─ coordinator 评估结果 → 再 dispatch 或输出最终响应 → 客户端
```

### 四个 task 的职责

| task | type | 节点 | 职责 |
|------|------|------|------|
| dispatcher (id=0) | dispatcher | 无（仅路由） | next_skills 指向 dummy_pre_skill，生成 dialogue_id，写 user_prompt 到 DataBus |
| dummy_pre_skill (id=2) | skill | dummy_pre_skill.py | 前置 hook 点，纯 passthrough，完成后 framework forward 给 coordinator |
| coordinator (id=1) | coordinator | coordinator_decide.py | LLM 决策核心：首次规划、评估 skill 结果、决定再 dispatch 或回复 |
| skill-runner (id=3) | skill | skill_executor.py | 通用 skill 执行器，通过 exec_skill_key 路由到任意 skill |

## 设计决策

### 1. 为什么用 dummy_pre_skill hook 而非 dispatcher 直接路由到 coordinator？

**结论：为了提供可扩展的 hook 点，同时利用 framework 的原始输入恢复机制。**

AgentFlow 框架的 dispatcher→coordinator 路径有两条：

| 路径 | 机制 | 原始输入保留 |
|------|------|------|
| A: dispatcher 有 next_skills → launch skill task | `launch_skill_task()` 启动 skill | ✅ 保留 message.content，写 user_prompt 到 DataBus |
| B: skill 完成 → forward to coordinator | `process_skill_response` forward | ✅ 从 `{dialogue_id}/user_prompt` 恢复（agent.cpp:1796） |

**dummy_pre_skill 的作用**：
1. 作为 dispatcher 的 next_skills 目标（走路径 A），framework 生成 dialogue_id 并写 `user_prompt` 到 DataBus
2. 完成后走路径 B（forward to coordinator），coordinator 从 `user_prompt` 恢复原始输入
3. 作为 hook 点，未来可扩展输入校验、鉴权、富化、遥测等前置处理

### 2. 为什么 LLM 决策放在 coordinator task 而非 coordinator-skill 中间层？

**结论：消除了 V2 架构的 coordinator-skill 中间层，coordinator task 直接承载 LLM 决策。**

V2 架构因框架限制（dispatcher 无法直接把原始输入传给 coordinator task）而引入 `coordinator-skill`（type=skill）中间层。V3 通过 dummy_pre_skill → forward 路径解决了输入传递问题，coordinator task 直接运行 `coordinator_decide.py`，不再需要中间层。

### 3. 为什么只有一个 skill-runner task（而非每个 skill 一个 task）？

**结论：exec_skill_key 路由 + DataBus dispatch manifest 实现"一个 task 跑任意 skill"。**

coordinator_decide 调 run_skills 时：
1. 把每个 skill 的真实 `skill_name` + `prompt` + `load_skills` 写入 DataBus `dispatch:{N}` manifest
2. 输出 `next_skills: [{name:"skill-runner", exec_skill_key:"dispatch:N", force:true}]`

框架通过 `AF_EXEC_SKILL_KEY` 环境变量把每个 skill-runner 实例路由到不同的 dispatch manifest。`force=true` 绕过框架的同名 skill 去重，允许并行多个 skill-runner 实例。

**新增 skill = 只加 SKILL.md + agent.md 注册一行，不改 agent.json。**

### 4. 为什么 tool 放在 skill 的 scripts/ 下（而非全局 harness/tools/）？

**结论：单一职责 + skill 自包含。**

每个 skill 自带 tool，co-located 在 `harness/skills/{skill_name}/scripts/`：

```
harness/skills/weather_query/
├── SKILL.md                    # skill 文档（LLM 读这个理解 skill 能力）
└── scripts/
    └── weather_query.py        # skill 专属 tool（skill_executor 从这里加载）
```

skill_executor.py 的 `_LocalToolRegistry(skill_name)` 按当前 skill_name 从对应 `scripts/` 目录加载 tool。skill 间 tool 隔离，避免串扰。

### 5. trace 可观测性

每次 LLM 调用通过 `llm_call.py` 的 `_TraceRecorder` 写入 DataBus `llm` 命名空间：

| DataBus key | 内容 |
|-------------|------|
| `llm/traces/{call_id}/request` | 完整 HTTP 请求体（messages + tools） |
| `llm/traces/{call_id}/response` | 完整 HTTP 响应体 |
| `llm/traces/{call_id}/meta` | latency、tokens、finish_reason、摘要 |
| `llm/traces/index` | call_id 有序列表，供枚举 |
| `llm/session_messages` | 跨触发的对话历史（session 级持久化） |

HTTP 传输用 `curl` 子进程（绕过 miniconda Python SSL 证书问题，curl 用系统钥匙串）。

### 6. 跨触发对话历史

coordinator_decide 每次触发只做一轮 LLM 决策，然后 yield control 等待 skill 完成。对话历史通过 `llm/session_messages` 在 DataBus 跨触发持久化：

- 首次触发（dummy_pre_skill forward）：读空历史 + 用户输入 → LLM 决策 → 写回历史
- skill 完成触发：读历史 + skill 结果通知 → LLM 评估 → 写回历史或输出最终回复

## 目录结构

```
outfit_assistant/
├── agent.json                          # 4 task 定义
├── agent.md                            # 业务意图 + model 配置 + skills 列表
├── package.json                        # workflow 注册
├── .env / .env.example                 # API key 配置
├── requirements.txt                    # Python 依赖
├── setup.sh                            # 环境初始化脚本
├── dummy_pre_skill_workflow.json       # dummy_pre_skill hook workflow
├── coordinator_workflow.json           # coordinator task workflow (coordinator_decide)
├── skill_workflow.json                 # skill-runner task workflow
├── nodes/
│   ├── dummy_pre_skill.py              # 前置 hook 占位节点
│   ├── coordinator_decide.py           # LLM 决策核心（run_skills 工具）
│   ├── skill_executor.py               # 通用 skill 执行器 + skill-scoped tool 加载
│   ├── databus_client.py               # DataBus 封装（dialogue_id 前缀对齐）
│   └── llm_call.py                     # LLM 调用 (curl) + trace 记录
├── harness/
│   ├── skills/
│   │   ├── weather_query/
│   │   │   ├── SKILL.md
│   │   │   └── scripts/weather_query.py    # QWeather API tool
│   │   ├── outfit-advisor/SKILL.md
│   │   ├── occasion-guide/SKILL.md
│   │   ├── dummy_pre_skill/SKILL.md        # hook 占位
│   │   └── skill-runner/SKILL.md           # 占位
│   └── tools/
│       ├── echo.py
│       └── profile_manager.py
└── data/
    └── occasion_guide.json
```

## 快速开始

### 1. 环境初始化

```bash
cd examples/agent/outfit_assistant
bash setup.sh                    # 创建 .venv + 装依赖
cp .env.example .env             # 填入真实 API key
$EDITOR .env                     # 填 QWEATHER_API_KEY / QWEATHER_API_HOST / API_KEY
```

### 2. 启动 daemon

在项目根目录执行：

```bash
cd /path/to/aisf_agentflow

# 加载 .env 环境变量
set -a; . examples/agent/outfit_assistant/.env; set +a

# 启动 daemon（PYTHON_PATH 必须用绝对路径，指向 .venv）
PYTHON_PATH="$(pwd)/examples/agent/outfit_assistant/.venv" \
  ./build/agentflowd \
  --listen 0.0.0.0:7891 \
  --package "$(pwd)/examples/agent/outfit_assistant"
```

> **关键**：`PYTHON_PATH` 必须是绝对路径。框架 worker 用这个路径找 `.venv/bin/python3` 执行节点脚本；相对路径会因 working_dir 切换而失效。

### 3. 测试

```bash
python3 examples/agent/harness_agent_v2/test_client.py \
  --port 7891 \
  --agent-id outfit-assistant \
  --message "今天北京天气如何"
```

预期输出包含真实天气数据（来自 QWeather API，非 LLM 编造）。

## 加新 skill

只需两步，不改任何代码：

```bash
# 1. 创建 skill 目录 + SKILL.md（+ 可选 scripts/ 带 tool）
mkdir -p harness/skills/my-skill/scripts
cat > harness/skills/my-skill/SKILL.md << 'EOF'
---
name: my-skill
description: 这个 skill 做什么
---

# My Skill

## 行为
- 具体能力描述

## 自带工具（可选）
- `my_tool`（scripts/my_tool.py）：tool 说明
EOF

# 2. 在 agent.md 的 skills 列表加一行
#    skills:
#      - my-skill
```

coordinator_decide 读 agent.md 的 skills 列表写进 system prompt，LLM 自主决定何时 dispatch 这个 skill。skill_executor 会从 `harness/skills/my-skill/scripts/` 自动加载 tool。

## 扩展 dummy_pre_skill hook

dummy_pre_skill 当前是纯 passthrough。未来可在此扩展前置处理：

```python
# nodes/dummy_pre_skill.py 示例扩展
def execute():
    user_input = os.environ.get("AF_INPUT", "")

    # 示例：输入校验
    if not user_input or len(user_input) > 10000:
        print(json.dumps({"error": "invalid input"}, ensure_ascii=False))
        return

    # 示例：敏感词过滤 / 鉴权 / 富化
    # processed = enrich(user_input)

    # 完成后 framework 自动 forward 给 coordinator（从 user_prompt 恢复原始输入）
    print(json.dumps({}, ensure_ascii=False))
```

**hook 契约**：
- 禁止输出 next_skills（否则走 launch 路径，coordinator 收不到原始输入）
- 禁止写对话历史（coordinator 拥有历史所有权）
- 可读 AF_INPUT 做前置处理，但 framework 传给 coordinator 的仍是原始 user_prompt

## 环境变量

| 变量 | 用途 | 必填 |
|------|------|------|
| `API_KEY` | LLM API key | 是 |
| `QWEATHER_API_KEY` | 和风天气 API key | weather_query skill 需要 |
| `QWEATHER_API_HOST` | 和风天气 API host | weather_query skill 需要 |
| `AF_LLM_BASE_URL` | LLM base URL（覆盖 agent.md） | 否 |
| `AF_LLM_MODEL` | LLM 模型名（覆盖 agent.md） | 否 |
| `PYTHON_PATH` | .venv 目录（框架 worker 用） | 启动 daemon 时必填 |

## 依赖

- `pyyaml>=6.0` — 解析 agent.md / SKILL.md 的 YAML frontmatter
- `python-dotenv>=1.0` — .env 加载
- `agentflow_databus` — 来自项目 `src/runtime/databus/clients/python`，由节点通过 `sys.path` 注入，无需 pip 安装
