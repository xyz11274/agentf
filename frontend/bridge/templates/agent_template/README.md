# Harness Agent 模版

> `harness_agent/` — Harness 运行时框架的标准模版

## 用途

当用户通过 `POST /api/packages/create-from-template` 创建新 Agent 时，
本模版提供完整的运行时脚手架（nodes、workflows、harness tools、tests），
用户只需提供 `agent.md`（agent identity）和自定义 skill。

## 模版内容

| 路径 | 所有权 | 说明 |
|------|--------|------|
| `agent.json` | 深度合并 | 4 个基础设施 task（dispatcher/coordinator/skill-runner/dummy_pre_skill） |
| `agent.md` | 用户胜出 | 最小 frontmatter（用户 identity 覆盖） |
| `package.json` | 用户胜出 | 包元数据 |
| `harness/tools/*` | 模版所有 | 运行时工具：bash、file I/O、MCP bridge 等 |
| `harness/skills/skill-runner/` | 模版所有 | 通用 skill 执行器 |
| `harness/skills/dummy_pre_skill/` | 模版所有 | 前置处理 hook |
| `nodes/coordinator_decide.py` | 用户可覆盖 | 协调器节点 |
| `nodes/skill_executor.py` | 用户可覆盖 | Skill 执行节点 |
| `nodes/databus_client.py` | 模版所有 | DataBus 客户端基础设施 |
| `nodes/llm_call.py` | 模版所有 | LLM 调用基础设施 |
| `nodes/dummy_pre_skill.py` | 模版所有 | 前置处理节点 |
| `coordinator_workflow.json` | 模版+路径重写 | 协调器 DAG |
| `skill_workflow.json` | 模版+路径重写 | Skill 执行 DAG |
| `dummy_pre_skill_workflow.json` | 模版+路径重写 | 前置处理 DAG |
| `tests/*` | 模版所有 | 测试框架 |
| `setup.sh` / `requirements.txt` / `.env.example` | 用户胜出 | 环境配置 |

## 用户最小上传

用户上传的压缩包只需包含：

```
my-agent/
├── agent.md            # frontmatter: name, model, description, skills
└── harness/skills/
    └── my-custom-skill/
        └── SKILL.md    # 自定义 skill 定义
```

模版会自动补齐所有运行时文件。

## 设计文档

详见 `docs/design/features/agent-template-merge.md`
