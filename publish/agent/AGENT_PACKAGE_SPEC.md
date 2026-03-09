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
- `tasks`：agent 级任务列表；当前要求包含 1 个 `contextupdater` 任务
- `sessions`：同质化 session 模板定义，固定只配置 1 个 session
  - `session.id`：固定为 `0`，表示模板编号
  - `session.description`
  - `session.task_count`
  - `session.tasks`
- `task.id`：从 `0` 开始连续编号
- 顶层 `tasks[].type`：当前固定为 `contextupdater`
- `session.tasks[].type`：`dispatcher` / `coordinator` / `skill`
- `task.workflow_file`：任务绑定的 workflow 文件
- `task.workflow_pool_size`：workflow 多实例池大小（>= 1，默认 1）；runtime 会按该值预创建 workflow 实例池
- `workflow` 中的 node 可声明 `execution_risk_level`（`green` / `yellow` / `red`）；coordinator 可据此决定 skill 是否直接执行
- `skill` 任务额外包含：
  - `skill.name`
  - `skill.config_dir`
  - `skill.instance_pool_size`（>= 1，默认 2）
- `workflow_pool_size` 与 `skill.instance_pool_size` 语义不同：
  - `workflow_pool_size` 描述 task 绑定 workflow 的实例池规模，并由 deployer / scheduler 在运行时建立 workflow 实例与 worker / task 的绑定；
  - `skill.instance_pool_size` 描述 skill task 自身应展开出的任务实例数。
- `dispatcher` / `coordinator` 是特殊 task：workflow 输出 `selected_skills` 后，agent 会继续把这些 skill 具象化为 runtime task。
- 每个 session 模板必须包含 1 个 `dispatcher`、1 个 `coordinator`，以及至少 1 个 `skill`

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
- 包名/版本/类型
- agent 版本
- 功能说明
- 文件清单（manifest）
- `coordinator_workflow`：可作为 coordinator 任务的默认 workflow
- `task_workflows`：任务类型到 workflow 的默认映射（如 `dispatcher` / `coordinator` / `contextupdater` / `skill:<name>`）

> 说明：session / task / skill 的主拓扑定义应放在 `agent.json`，`package.json` 负责包级元数据与默认 workflow 映射。

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
