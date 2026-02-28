# Agent 包规范说明

## 1. 包命名规范

- 外层发布包：`$xxxAgent_$PKGTYPE_$PKGVER.tar.gz`
- 内层 Agent 对象包：`$xxxAgent_$AGTVER.tar.gz`

字段含义：
- `$xxxAgent`：Agent 名称（建议以 `Agent` 结尾）
- `$PKGTYPE`：包类型，例如 `release` / `snapshot`
- `$PKGVER`：发布包版本
- `$AGTVER`：Agent 对象版本

## 2. 包结构

```text
$xxxAgent_$PKGTYPE_$PKGVER.tar.gz
├── $xxxAgent_$AGTVER.tar.gz
└── package.json
```

内层 Agent 对象包内容：

```text
$xxxAgent_$AGTVER.tar.gz/
├── agent.json
├── agentCard.md
├── workflow_XX.json
└── node_xxx.$TYPE
```

## 3. 必需文件说明

### 3.1 `agent.json`
Agent 运行时元数据，至少包含：
- `name`：Agent 名称
- `version`：Agent 版本（应与 `$AGTVER` 一致）
- `description`：功能概述
- `workflows`：工作流文件列表

### 3.2 `agentCard.md`
Agent Prompt/能力卡片，供其他模型或 Agent 读取。

### 3.3 `workflow_XX.json`
工作流定义文件，包含 `node` 与 `edge`：
- node 负责动作执行
- edge 负责流程控制（包含 if/loop/fork/join/async/goto）
- coordinator workflow 推荐包含 session 创建节点与 task 创建节点，作为 task0 的固定编排入口

### 3.4 `node_xxx.$TYPE`
节点可执行实现，支持后缀示例：`.so`、`.sh`、`.py`、`.jar`。

### 3.5 `package.json`
外层发布包描述文件，建议包含：
- 包名/版本/类型
- agent 版本
- 功能说明
- 文件清单（manifest）
- `coordinator_workflow`：固定指定 coordinator（task0）默认 workflow
- `task_workflows`：任务类型到 workflow 的默认映射（用于非 task0 或兼容场景）

## 4. 校验清单

- 外层/内层包文件名需符合命名规范
- `agent.json.version` 必须与内层包版本一致
- `package.json.version` 必须与外层包版本一致
- `package.json.file_manifest` 必须包含内层 tarball 与 `package.json`
