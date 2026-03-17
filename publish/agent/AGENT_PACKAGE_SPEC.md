# MMAgent 包规范说明

## 1. 包命名规范

- 外层发布包：`$xxxAgent_$PKGTYPE_$PKGVER.tar.gz`
- 内层 MMAgent 对象包：`$xxxAgent_$AGTVER.tar.gz`

字段含义：
- `$xxxAgent`：MMAgent 名称（建议以 `MMAgent` 结尾）
- `$PKGTYPE`：包类型，例如 `release` / `snapshot`
- `$PKGVER`：发布包版本
- `$AGTVER`：MMAgent 对象版本

## 2. 包结构

```text
$xxxAgent_$PKGTYPE_$PKGVER.tar.gz
├── $xxxAgent_$AGTVER.tar.gz
└── package.json
```

内层 MMAgent 对象包内容：

```text
$xxxAgent_$AGTVER.tar.gz/
├── agent.json
├── agentCard.md
├── workflow_XX.json
└── node_xxx.$TYPE
```

## 3. 必需文件说明

### 3.1 `agent.json`
MMAgent 运行时元数据，至少包含：
- `name`：MMAgent 名称
- `version`：MMAgent 版本（应与 `$AGTVER` 一致）
- `description`：功能概述
- `session_pool_size`：预创建 session 池大小（>= 1），按同一份 session 模板实例化
- `max_workflows`：agent 允许装载的 workflow 上限（>= 1）
- `max_concurrent_workflows`：agent 允许的并发 workflow 上限（>= 1）
- `tasks`：任务列表，通过 `scope` 属性区分层级：
  - `scope: "agent"`：Agent 级任务（如 `contextupdater`）
  - `scope: "session"` 或缺省：Session 级任务（`dispatcher` / `coordinator` / `skill`）
- `task.id`：从 `0` 开始连续编号
- `task.scope`：任务作用域，`"agent"` 或 `"session"`（默认 `"session"`）
- `task.type`：任务类型
  - Agent 级：`contextupdater`
  - Session 级：`dispatcher` / `coordinator` / `skill`
- `task.workflow_file`：任务绑定的 workflow 文件
  - 对于 `dispatcher` 类型，`workflow_file` 为可选字段（Dispatcher 是框架内置 workflow）
- `task.workflow_pool_size`：workflow 多实例池大小（>= 1，默认 1）；runtime 会按该值预创建 workflow 实例池
- `workflow` 中的 node 可声明 `execution_risk_level`（`green` / `yellow` / `red`）；coordinator 可据此决定 skill 是否直接执行
- `skill` 任务额外包含：
  - `skill.name`
  - `skill.config_dir`
  - `skill.instance_pool_size`（>= 1，默认 2）
- **`dispatcher` 任务特定字段**（Dispatcher 为框架内置 workflow）：
  - `next_skills`：字符串数组，指定 Dispatcher 默认拉起的 skill 名称列表
  - `max_concurrent_skills`：最大并发 skill 数（>= 1，默认 10）
- **next_skills 机制**：
  - 任何 task（dispatcher/coordinator/skill）都可以输出 `next_skills` 到 DataBus
  - 框架会统一检测并拉起对应的 skill task
  - 输出格式：JSON 数组 `["skill1", "skill2"]` 或逗号分隔字符串 `"skill1,skill2"`
- **skill workflow 输出约定**：
  - `response`：skill 执行结果文本
  - `next_skills`：后续要拉起的 skill 名称列表
  - `skill_completed`：布尔值，表示 skill 是否完成
- `workflow_pool_size` 与 `skill.instance_pool_size` 语义不同：
  - `workflow_pool_size` 描述 task 绑定 workflow 的实例池规模，并由 deployer / scheduler 在运行时建立 workflow 实例与 worker / task 的绑定；
  - `skill.instance_pool_size` 描述 skill task 自身应展开出的任务实例数。
- `dispatcher` / `coordinator` 是特殊 task：workflow 输出 `selected_skills` 后，agent 会继续把这些 skill 具象化为 runtime task。
- 每个 session 模板必须包含 1 个 `dispatcher`、1 个 `coordinator`，以及至少 1 个 `skill`

#### 3.1.1 扁平化结构示例

```json
{
  "name": "BasicAgent",
  "version": "1.0.0",
  "description": "Basic execution agent example",
  "session_pool_size": 2,
  "max_workflows": 10,
  "max_concurrent_workflows": 5,
  "tasks": [
    {
      "id": 0,
      "type": "contextupdater",
      "scope": "agent",
      "description": "Update agent-level context",
      "workflow_file": "workflow_01.json",
      "workflow_pool_size": 2
    },
    {
      "id": 1,
      "type": "dispatcher",
      "description": "Dispatch user requests",
      "workflow_file": "dispatcher_workflow.json",
      "workflow_pool_size": 3,
      "next_skills": ["skill1", "skill2"],
      "max_concurrent_skills": 10
    },
    {
      "id": 2,
      "type": "coordinator",
      "description": "Coordinate task outputs",
      "workflow_file": "coordinator_workflow.json",
      "workflow_pool_size": 2
    },
    {
      "id": 3,
      "type": "skill",
      "description": "Knowledge skill",
      "workflow_file": "knowledge_workflow.json",
      "skill": {
        "name": "knowledge",
        "config_dir": "skills/knowledge",
        "instance_pool_size": 2
      },
      "workflow_pool_size": 2
    }
  ]
}
```

#### 3.1.2 向后兼容

框架支持旧版 `sessions` 数组格式的向后兼容解析：
- 如果存在顶层 `sessions` 数组，则按旧格式解析
- 如果不存在 `sessions` 数组，则按新的扁平化格式解析

旧格式示例（已废弃，仅用于向后兼容）：
```json
{
  "tasks": [
    { "id": 0, "type": "contextupdater", "scope": "agent", ... }
  ],
  "sessions": [
    {
      "id": 0,
      "tasks": [
        { "type": "dispatcher", ... },
        { "type": "coordinator", ... },
        { "type": "skill", ... }
      ]
    }
  ]
}
```

### 3.2 `agentCard.md`
MMAgent Prompt/能力卡片，供其他模型或 MMAgent 读取。

### 3.3 `workflow_XX.json`
工作流定义文件，包含 `node` 与 `edge`：
- node 负责动作执行
- edge 负责流程控制（包含 if/switch-case）
- session pool 中每个 session 都由同一份模板复制而来，并固定包含 1 个 dispatcher、1 个 coordinator 和若干 skill 任务
- agent 级 context workflow 应通过顶层 `tasks[]` 中的 `contextupdater` 任务声明

### 3.4 `node_xxx.$TYPE`
节点可执行实现，支持后缀示例：`.so`、`.sh`、`.py`、`.jar`。

### 3.5 `package.json`
外层发布包描述文件，建议包含：
- `name`：包名
- `version`：包版本
- `description`：功能说明
- `author`：作者信息
- `entry_point`：入口 SO 文件名
- `workflows`：要加载的 workflow 文件列表
- `builtin_workflows`：内置 workflow 名称列表（如 `memory`）
- `dependencies`：依赖声明
- `metadata`：自定义元数据

> 说明：session / task / skill 的主拓扑定义放在 `agent.json`，`package.json` 负责包级元数据与 workflow 文件加载列表。task 与 workflow 的绑定关系由 `agent.json` 的 `tasks[].workflow_file` 定义，运行时自动构建映射。

### 3.6 运行时消息约定

- `SessionRouter` 按 `user_id + client_id` 复用/分配 pooled session。
- session 自身不再维护收发线程；dispatcher / coordinator task 各自拥有：
  - `intent_request` 队列：向 runtime worker 提交 workflow 请求
  - `data_exchange` 队列：接收 worker 回包并继续协同编排
- worker 统一采用 `control` / `business` 双队列模型；workflow / node / edge 不再暴露独立事件线程。

## 4. 校验清单

- 外层/内层包文件名需符合命名规范
- `agent.json.version` 必须与内层包版本一致
- `package.json.version` 必须与外层包版本一致
- `package.json.file_manifest` 必须包含内层 tarball 与 `package.json`
