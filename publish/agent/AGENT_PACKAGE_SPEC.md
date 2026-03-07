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
- `session_pool_size`：预创建 session 池大小，需与 `sessions` 数组长度一致
- `contextupdater`：agent 级 context 更新任务配置（`type = contextupdater`，绑定 workflow 文件）
- `sessions`：session 池模板定义，至少包含 1 个 session
  - `session.id`：从 `0` 开始连续编号
  - `session.description`
  - `session.task_count`
  - `session.tasks`
- `task.id`：从 `0` 开始连续编号
- `task.type`：`dispatcher` / `coordinator` / `skill`
- `task.workflow_file`：任务绑定的 workflow 文件
- `skill` 任务额外包含：
  - `skill.name`
  - `skill.config_dir`
  - `skill.instance_pool_size`（>= 1）

### 3.2 `agentCard.md`
MMAgent Prompt/能力卡片，供其他模型或 MMAgent 读取。

### 3.3 `workflow_XX.json`
工作流定义文件，包含 `node` 与 `edge`：
- node 负责动作执行
- edge 负责流程控制（包含 if/loop/fork/join/async/goto）
- session pool 中每个 session 的 dispatcher / coordinator workflow 推荐分别承担请求分发与结果编排入口
- context workflow 应收敛到 agent 级 `contextupdater` 任务

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

## 4. 校验清单

- 外层/内层包文件名需符合命名规范
- `agent.json.version` 必须与内层包版本一致
- `package.json.version` 必须与外层包版本一致
- `package.json.file_manifest` 必须包含内层 tarball 与 `package.json`
