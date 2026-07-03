# Gap 03: agent.md 解析支持缺失

## 状态
❌ **未实现**

## 问题描述

当前框架中**不存在任何 agent.md 的解析路径**。项目中的 `agentCard.md` 只是一个给人看的说明卡片（basic-agent 中有），框架根本不读它。agent 配置完全依赖手写 JSON。

## 当前方式 vs 目标方式

### 当前（手写 JSON）

```
examples/agent/harness-agent/
├── agent.json              ← 手写 task 定义
├── package.json            ← 手写元数据
├── coordinator_workflow.json ← 手写 workflow
├── dispatcher_pass_through.json
└── nodes/*.py              ← 手写节点脚本
```

### 目标（agent.md → 自动生成）

```
my-agent/
├── agent.md                ← 用户只写这个 Markdown
│   ├── YAML frontmatter    ← model、skills 声明
│   └── Markdown body       ← system prompt、行为描述
        ↓ agenmd build
├── agent.json              ← 自动生成
├── daemon_config.json      ← 自动生成
├── workflow.json(s)        ← 自动生成
├── package.json            ← 自动生成
└── nodes/*.py              ← 复制通用模板
```

## 需要实现的组件

| 组件 | 说明 | 优先级 |
|------|------|--------|
| `AgentMDConfig` 数据模型 | 定义 YAML frontmatter 的 schema | P0 |
| `parse_yaml_frontmatter()` | 解析 YAML frontmatter 段 | P0 |
| `extract_markdown_body()` | 提取 Markdown body 作为 system prompt | P0 |
| `agenmd build` CLI | 核心生成器：agent.md → JSON 配置 | P0 |
| `agenmd init` CLI | 模板生成器：创建 agent.md 骨架 | P1 |
| `agenmd validate` CLI | 格式校验 | P1 |
| `AF_SYSTEM_PROMPT` 注入 | Worker 侧新增环境变量注入 | P0 |

## 影响

- 用户必须手写复杂的 JSON 配置
- agent 配置门槛高，不利于生态扩展
- system prompt 硬编码在 Python 脚本中，不易管理

## 参考

- `docs/usage/plan/agent.md支持方案分析.md` — 完整方案设计（含格式定义、映射规则、实现步骤）
- `docs/usage/plan/1.方案和gap分析.md:620-630` — 加载链路分析和 gap 发现
- `examples/agent/harness-agent/agentCard.md` — 现有的人肉说明卡片（框架不解析）
