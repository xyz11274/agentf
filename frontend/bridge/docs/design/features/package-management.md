# Agent Package 管理功能设计

> **层级**: L2 · `docs/design/features/package-management.md`
> **状态**: 已填充 · 正文由 `design-detailed-design-doc-writer` 撰写
> **创建者**: `design-doc-organizer`
> **填充日期**: 2026-06-25

---

## What

Agent Package 管理模块提供对 Agent Package 文件系统的完整 CRUD 操作，以及构建（生成 `.tar.gz` 分发包）和上传/下载能力。与前几个模块不同，本模块直接操作文件系统，不经过 daemon。

## Why

- Vue 前端的文件浏览器需要服务端提供文件树、文件内容读写等能力
- Agent Package 的打包/构建是一个文件系统任务，不需要 daemon 参与
- 上传/下载需要服务端文件系统权限
- 文件操作需要统一的安全校验（防止路径遍历攻击）

## How

### 功能领域

| 功能领域 | 说明 | 不经过 daemon |
|----------|------|--------------|
| 元数据查询 | 列出所有 Package、获取单个 Package 元信息 | ✅ |
| 文件浏览 | 获取 Package 文件树、读取/写入文件内容 | ✅ |
| 文件操作 | 创建/删除文件、创建/删除目录、重命名文件 | ✅ |
| 包管理 | 创建新 Package（空目录）、上传归档解压 | ✅ |
| 构建分发 | 构建 Package 为 `.tar.gz` 分发包、下载已构建包 | ✅ |

### 安全模型

文件系统操作的安全校验分为三层：

```
第一层：包名校验
  _validate_package_name(name)
  └─ 仅允许 [a-zA-Z0-9_-]，拒绝 ".."、"/"、空格等

第二层：相对路径校验
  _validate_relative_path(rel_path)
  └─ 拆分为 Posix 段，拒绝 ".."、"/" 开头、空段
  └─ 返回安全分段列表

第三层：目录边界校验
  _resolve_within(package_dir, segments)
  └─ package_dir / *segments → resolve()
  └─ 校验 resolved 路径的 parent 是否为 package_dir
  └─ 越界 → 403 Forbidden
```

所有端点强制使用这三层校验后方可操作文件系统。

### 文件模型

Package 被视为一个文件树，根目录为 `{agent_packages_path}/{package_name}/`。每个节点是 `FileNode`：

| 字段 | 类型 | 说明 |
|------|------|------|
| `name` | string | 文件名或目录名 |
| `path` | string | API 路径（非文件系统路径） |
| `type` | `"file"` 或 `"dir"` | 节点类型 |
| `children` | array | 目录时递归包含子节点 |
| `content` | string | 文件被读取时填充 |

### 构建流程

```
源码目录                         构建产物
{agent_packages_path}/          {source}/dist/
  {name}/                         {name}-{sha256[:12]}.tar.gz
    ├── agent.json
    ├── package.json
    ├── skills/
    └── nodes/
```

1. Bridge 调用 `agentflow` 子进程执行构建命令
2. 构建产物输出到 `<package>/dist/<name>-<sha256>.tar.gz`
3. 支持同一 Package 的多个版本（不同 sha256）
4. 子进程超时 60s

### 上传流程

1. 客户端以 `multipart/form-data` 提交归档文件
2. Bridge 校验文件后缀（仅接受 `.zip` / `.tar.gz` / `.tgz`）
3. 从文件名推断包名（或使用显式传入的 `name`）
4. 校验包名合法性，检查是否已存在
5. 将上传文件保存到临时位置
6. 解压到目标 Package 目录
7. 解压完成后删除临时文件
8. **自动初始化环境**：若 Package 根目录存在 `setup.sh`，则异步执行以提前创建虚拟环境并安装依赖（best-effort，失败不阻断上传）
9. 返回新 Package 的 `PackageInfo`

### 更新流程

1. 客户端以 `multipart/form-data` 提交归档文件 + 更新模式（`full` / `incremental`）
2. Bridge 校验文件后缀和模式参数
3. 校验包名合法性，检查 Package 是否已存在
4. 将上传文件保存到临时位置
5. `full` 模式：清空现有内容（保留 `dist/` 目录）
6. 解压到目标 Package 目录，覆盖/新增文件
7. 解压完成后删除临时文件
8. **自动初始化环境**：若 Package 根目录存在 `setup.sh`，则异步执行以提前创建虚拟环境并安装依赖（best-effort，失败不阻断更新）
9. 尝试热重载 Agent（若已在 daemon 中加载）
10. 返回更新后的 `PackageInfo`

### PackageInfo 模型

| 字段 | 类型 | 说明 |
|------|------|------|
| `name` | string | 包名（目录名） |
| `path` | string | 包在文件系统中的绝对路径 |
| `file_count` | int | 文件数量（递归统计，不含目录） |
| `last_modified` | string | ISO 8601 格式的最后修改时间戳 |

### 不支持项 / 边界

| 场景 | 状态 | 说明 |
|------|------|------|
| 文件搜索/查找 | ❌ 不支持 | 无全文搜索能力 |
| 二进制文件编辑 | ❌ 不支持 | 仅支持文本文件的读写 |
| 大文件上传 | ⚠️ 限制 | 受 FastAPI UploadFile 内存限制，无显式大小上限 |
| 并发写入保护 | ❌ 不支持 | 无文件锁，并发写入可能冲突 |

---

**接口契约（端点路径、请求/响应体、上传格式、安全校验规则）请参见 `architecture/modules/package-api.md`**

**相关设计文档**:
- `docs/design/features/bridge-overview.md` — Bridge 总体架构
- `docs/design/architecture/modules/package-api.md` — Package API 接口契约（L4）
