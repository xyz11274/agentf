# Agent Template Merge — 设计文档

> **文档层级**: L2 领域设计  
> **状态**: 初稿  
> **作者**: AgentFlow Bridge  
> **日期**: 2026-07-03

---

## 1. 问题

用户创建新 Harness Agent 时，需要完整的运行时脚手架（`harness/tools/*`、`nodes/*`、
`coordinator_workflow.json`、`skill_workflow.json`、`tests/` 等），但**用户只关心**自己
的业务逻辑（`agent.md` 中的 identity、自定义 skill 实现）。

当前只有两种创建方式：
- `POST /api/packages/create` — 空目录，用户需要手动创建所有文件
- `POST /api/packages/upload` — 解压用户上传的压缩包，但无模板叠加能力

两种方式均无法让用户"上传最小 agent 定义包 + 自动补齐运行时框架"。

---

## 2. 设计目标

| 目标 | 优先级 |
|------|--------|
| 用户只需提供 agent identity（`agent.md`）和业务 skill | P0 |
| 模板自动补齐运行时基础设施，不可被用户误覆盖 | P0 |
| 模板中的 `working_dir` 自动重写为新的 package 路径 | P0 |
| 用户可提供自定义 `agent.json` 字段覆盖（name/version/description） | P1 |
| 用户可新增自定义 task（追加到模板 task 列表之后） | P2 |
| 模板可插拔 — 支持多模板（future） | P2 |

---

## 3. 模板结构

模板位于 `templates/harness_agent/`：

```
harness_agent/
├── agent.json                          # 4 个基础设施 task（dispatcher/coordinator/skill-runner/dummy_pre_skill）
├── agent.md                            # 最小 frontmatter（被用户覆盖）
├── package.json                        # 包元数据（被用户覆盖）
├── .env.example                        # 环境变量模板
├── requirements.txt                    # Python 依赖
├── setup.sh                            # 环境初始化脚本
├── coordinator_workflow.json           # 协调器 DAG（working_dir 自动重写）
├── skill_workflow.json                 # Skill 执行 DAG（working_dir 自动重写）
├── dummy_pre_skill_workflow.json       # 前置处理 DAG（working_dir 自动重写）
├── harness/
│   ├── tools/                          # 运行时工具（模版所有，不覆写）
│   │   ├── bash.py, list_skills.py, load_skill.py, …
│   │   └── read_file.py, write_file.py, web_fetch.py, mcp_bridge.py
│   └── skills/
│       ├── skill-runner/SKILL.md       # 通用 skill 执行器（模版所有）
│       └── dummy_pre_skill/SKILL.md    # 前置处理 hook（模版所有）
├── nodes/
│   ├── coordinator_decide.py           # 协调器节点（用户可覆盖）
│   ├── skill_executor.py              # Skill 执行节点（用户可覆盖）
│   ├── llm_call.py                     # LLM 调用基础设施（模版所有）
│   ├── databus_client.py               # DataBus 客户端（模版所有）
│   └── dummy_pre_skill.py             # 前置处理节点（模版所有）
├── tests/                              # 测试框架（模版所有）
└── outputs/                            # 运行时输出目录
```

---

## 4. Merge 策略

### 4.1 所有权规则

| 类别 | 胜出方 | 文件/路径 | 说明 |
|------|--------|-----------|------|
| 运行时基础设施 | **模版** | `harness/tools/*`、`nodes/databus*`、`nodes/llm_call*`、`nodes/dummy_pre_skill*`、`tests/`、`harness/skills/skill-runner/`、`harness/skills/dummy_pre_skill/` | 用户上传的同名文件被丢弃，模版文件重新写回 |
| 用户 identity | **用户** | `agent.md`、`package.json`、`.env.example`、`requirements.txt`、`setup.sh` | 用户文件直接覆盖模版默认值 |
| 智能合并 | **深度合并** | `agent.json` | 模版的 `tasks[]` 保留，用户的 top-level 字段覆盖同名 |
| Workflow DAG | **模版+路径重写** | `coordinator_workflow.json`、`skill_workflow.json`、`dummy_pre_skill_workflow.json` | 模版提供 DAG，`working_dir` 自动重写为 package 路径 |
| 用户补充 | **用户添加** | `harness/skills/<custom>/` | 用户自定义 skill 加入已有目录 |

### 4.2 Merge 流程

```
Phase 1: Copy Template
  template_dir/ ──→ target_dir/     完整拷贝模版

Phase 2: Rewrite working_dir
  扫描 workflow JSON 文件，将 working_dir 从原始路径
  重写为当前 package 的相对路径

Phase 3: Extract User Archive
  user archive ──→ target_dir/      解压覆盖
  → reapply template-only files     模版基础设施重新写回

Phase 4: Deep-merge agent.json
  template tasks (保留)  +  user top-level fields (override)
  → 写回 target_dir/agent.json

Phase 5: Restore execute bits
  .sh, .py 文件添加可执行权限
```

### 4.3 `agent.json` 深度合并规则

```
输入:
  template_agent.json:  4 个基础设施 task (id=0,1,2,3)
  user_agent.json:      name="MyAgent", tasks=[{id: 10, ...}]

合并结果:
  name = "MyAgent"                       ← user 覆盖
  tasks = [                              ← 合并
    {id: 0, type: "dispatcher", ...},    ← 模版保留
    {id: 1, type: "coordinator", ...},   ← 模版保留
    {id: 2, type: "skill", ...},         ← 模版保留
    {id: 3, type: "skill", ...},         ← 模版保留
    {id: 10, ...}                        ← user 新增
  ]
  session_pool_size = user value         ← user 可选覆盖
  version = user value                   ← user 可选覆盖
```

可被用户覆盖的 top-level 字段白名单：
- `name`
- `version`
- `description`
- `session_pool_size`
- `max_workflows`
- `max_concurrent_workflows`

---

## 5. API 设计

### `POST /api/packages/create-from-template`

**请求** (`multipart/form-data`):

| 字段 | 类型 | 必填 | 默认 | 说明 |
|------|------|------|------|------|
| `file` | UploadFile | ✅ | — | 用户压缩包 (.zip / .tar.gz) |
| `name` | string | ⬜ | 文件名（去后缀） | 目标 package 名 |
| `template` | string | ⬜ | `harness_agent` | 模版目录名 |

**返回**: `PackageInfo` (201 Created)

**错误码**:

| HTTP | 条件 |
|------|------|
| 400 | 压缩包格式不支持 / 模版不存在 / package 名非法 |
| 409 | package 名已存在 |
| 500 | 文件系统错误 |

### 与现有 API 的关系

| 端点 | 关系 |
|------|------|
| `POST /api/packages/create` | 创建空目录（对需完全从零搭建的用户） |
| `POST /api/packages/upload` | 纯解压（对已有完整 agent 目录的用户） |
| `POST /api/packages/create-from-template` | **模版合并**（对首次创建、需要脚手架的用户） |

---

## 6. 配置

`config.yaml` 新增字段：

```yaml
agent_template_dir: "templates"
```

代码中 `settings.agent_template_path` 提供解析后的绝对路径。

---

## 7. 边界情况

| 场景 | 行为 |
|------|------|
| 用户上传空压缩包 | 模版完整拷贝，无用户覆盖 |
| 用户上传同名 `agent.md` | 用户版本覆盖模版版本（用户 identity 优先） |
| 用户上传 `user_customs/xxx.py` | 保留在 target 目录（用户补充） |
| 用户上传的文件覆盖了 `harness/tools/bash.py` | 模版重新写回，用户版本丢弃 |
| `agent.json` 深度合并时用户 task ID 冲突 | 模版 task 保留，用户 task 跳过 |
| 模版 workflow 文件中 `working_dir` 不存在于新路径 | 搜索替换，找不到则略过 |
| 包名与模版重名 | 409 Conflict（包名唯一） |
| 模版目录被删除或未找到 | 400 Bad Request + 列出可用模版 |

---

## 8. 未纳入范围

- 多模版选择管理（future：`template_browser` API）
- 模版版本管理（future：模版也带 `version` 字段，兼容性检查）
- 模版自定义（future：允许用户 fork 模版）
- 前端 UI 中的模版预览和选择

---

## 9. ADR

| 编号 | 决策 | 理由 |
|------|------|------|
| ADR-001 | 所有权规则用硬编码 tuple 而非配置文件 | KISS：所有权规则很少变化，JSON schema 解析的开销 > 收益 |
| ADR-002 | `agent.json` 深度合并不用 JSON Merge Patch | 需要保留模版 task 的精确结构，Merge Patch 会移除未被用户列出的 task |
| ADR-003 | template-only 文件在用户解压后重新写回 | 避免"单点发散"：如果只在解压前拒绝，模版修改后旧内容残留 |
| ADR-004 | `working_dir` 重写用字符串替换而非 JSON 遍历 | 所有 workflow 文件的 working_dir 在模版中是一致的，且 JSON 格式固定 |
