# Package 管理 API 接口契约

> **层级**: L4 · `architecture/modules/package-api.md`
> **状态**: 已填充 · 正文由 `design-detailed-design-doc-writer` 撰写
> **创建者**: `design-doc-organizer`
> **填充日期**: 2026-06-25

---

## What

本文档定义 Agent Package 管理模块的完整 API 接口契约，包括所有 CRUD 端点、上传/下载接口、构建接口的请求/响应体 Schema 和安全校验规则。

与前几个模块不同，本模块直接操作文件系统，不经过 daemon。

---

## 1. 全局约定

### 1.1 响应信封

所有端点统一返回 `ApiResponse` 格式（`{success, data, error}`）。文件下载端点直接返回二进制流。

### 1.2 安全校验规则

| 校验 | 规则 | 实现 |
|------|------|------|
| 包名校验 | 仅允许 `[a-zA-Z0-9_-]+` | `_validate_package_name()` |
| 相对路径校验 | 不允许 `..` 穿越，不允许绝对路径 | `_validate_relative_path()` |
| 目录边界校验 | 最终解析路径必须在 `agent_packages_path` 范围内 | `_resolve_within()` |
| 根目录逃逸 | 包名含 `..` 段时返回 403 | `_package_source_dir()` |

拒绝 path traversal 攻击的流程：

```
输入 path 字符串
  → _validate_relative_path(): 拆分为 Posix 段，拒绝 ".." 和 "/" 开头
  → _resolve_within(pkg_dir, segments): Path.resolve() 后校验 parent 关系
  → 越界则 403 Forbidden
```

### 1.3 PackageInfo 模型

```json
{
  "name": "my-agent",
  "path": "/home/user/AgentFlow/examples/agent/my-agent",
  "file_count": 12,
  "last_modified": "2026-06-25T15:00:00+08:00"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `name` | string | 包名（目录名） |
| `path` | string | 绝对路径 |
| `file_count` | int | 文件数量（递归，不含目录） |
| `last_modified` | string | ISO 8601 格式的最后修改时间 |

---

## 2. API 端点

### 2.1 列出所有 Package

```
GET /api/packages
```

**说明**: 返回 `agent_packages_path` 下的所有一级子目录作为 Package。

**响应 `data`**: `PackageInfo[]`

| 状态码 | 条件 |
|--------|------|
| 200 | 成功（可能为空数组） |

---

### 2.2 获取单个 Package

```
GET /api/packages/{name}
```

**路径参数**:

| 参数 | 类型 | 说明 |
|------|------|------|
| `name` | string | Package 名称 |

**响应 `data`**: `PackageInfo`

| 状态码 | 条件 |
|--------|------|
| 200 | 成功 |
| 403 | 非法包名 |
| 404 | Package 不存在 |

---

### 2.3 获取 Package 文件树

```
GET /api/packages/{name}/tree
```

**说明**: 返回 Package 目录的递归文件树。

**响应 `data`**: `FileNode[]`

`FileNode` 结构：

```json
{
  "name": "agent.json",
  "path": "/api/packages/{name}/file/agent.json",
  "type": "file",
  "children": null,
  "content": null
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `name` | string | 文件名或目录名 |
| `path` | string | 文件在 API 中的路径（可拼接为完整 API URL） |
| `type` | string | `"file"` 或 `"dir"` |
| `children` | array | 目录时递归包含子节点；文件时为 `null` |
| `content` | string | 仅在文件被读取时存在，默认 `null` |

---

### 2.4 读取文件内容

```
GET /api/packages/{name}/file/{file_path:path}
```

**说明**: 读取 Package 中指定文件的文本内容。

| 状态码 | 条件 |
|--------|------|
| 200 | 成功，返回文件内容（纯文本） |
| 403 | 路径越界 |
| 404 | 文件不存在 |

---

### 2.5 写入/覆盖文件

```
PUT /api/packages/{name}/file/{file_path:path}
```

**请求体**:

```json
{
  "content": "文件内容文本"
}
```

`FileWriteRequest`：

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `content` | string | 是 | 写入的文件内容 |

**说明**: 如果文件不存在则创建；如果存在则覆盖。中间缺失的父目录会被创建。

| 状态码 | 条件 |
|--------|------|
| 200 | 写入成功 |
| 403 | 路径越界 |

---

### 2.6 重命名文件/目录

```
POST /api/packages/{name}/file/{file_path:path}
```

**请求体**:

```json
{
  "new_name": "new-filename.json"
}
```

`FileRenameRequest`：

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `new_name` | string | 是 | 新文件名（仅文件名，不包含路径） |

| 状态码 | 条件 |
|--------|------|
| 200 | 重命名成功，返回更新后的文件信息 |
| 403 | 路径越界或非法文件名 |
| 404 | 源文件不存在 |
| 409 | 目标文件名已存在 |

---

### 2.7 创建空文件

```
POST /api/packages/{name}/file/{file_path:path}
```

（注意：此端点和重命名使用同一路径，由请求体区分——无 body 时创建空文件。）

| 状态码 | 条件 |
|--------|------|
| 201 | 创建成功 |
| 403 | 路径越界 |
| 409 | 文件已存在 |

---

### 2.8 删除文件

```
DELETE /api/packages/{name}/file/{file_path:path}
```

| 状态码 | 条件 |
|--------|------|
| 200 | 删除成功 |
| 403 | 路径越界 |
| 404 | 文件不存在 |

---

### 2.9 创建目录

```
POST /api/packages/{name}/dir
```

**请求体**:

```json
{
  "path": "subdir/nested"
}
```

`DirectoryCreateRequest`：

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `path` | string | 是 | 待创建目录路径（相对于 Package 根） |

| 状态码 | 条件 |
|--------|------|
| 201 | 创建成功 |
| 403 | 路径越界 |

---

### 2.10 删除目录

```
DELETE /api/packages/{name}/dir
```

**查询参数**:

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `path` | string | 是 | 待删除目录路径（相对于 Package 根） |

**说明**: 递归删除整个目录树。

| 状态码 | 条件 |
|--------|------|
| 200 | 删除成功 |
| 403 | 路径越界或试图删除 Package 根目录 |
| 404 | 目录不存在 |

---

### 2.11 创建空 Package

```
POST /api/packages/create
```

**请求体**:

```json
{
  "name": "new-agent"
}
```

`PackageCreateRequest`：

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `name` | string | 是 | 包名（1-128 字符，仅允许 `[a-zA-Z0-9_-]`） |

| 状态码 | 条件 |
|--------|------|
| 201 | 创建成功，返回 `PackageInfo` |
| 403 | 非法包名 |
| 409 | 同名 Package 已存在 |

---

### 2.12 上传归档解压为 Package

```
POST /api/packages/upload
```

**请求格式**: `multipart/form-data`

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `file` | file | 是 | 归档文件（`.zip` / `.tar.gz` / `.tgz`） |
| `name` | string | 否 | 包名；省略时从文件名推断（去掉扩展名） |

**包名推断规则**：

| 原始文件名 | 推测的包名 |
|-----------|-----------|
| `my-agent.zip` | `my-agent` |
| `my-agent.tar.gz` | `my-agent` |
| `my-agent.tgz` | `my-agent` |

**支持的归档格式**：

| 后缀 | 解压方式 |
|------|----------|
| `.zip` | `zipfile.ZipFile.extractall` |
| `.tar.gz` | `tarfile.open(…, "r:gz").extractall` |
| `.tgz` | 同 `.tar.gz` |

| 状态码 | 条件 |
|--------|------|
| 201 | 上传并解压成功，返回 `PackageInfo`（含 file_count） |
| 400 | 文件名为空、格式不支持、归档损坏 |
| 403 | 非法包名 |
| 409 | 同名 Package 已存在 |
| 500 | 保存临时文件失败 |

---

### 2.13 构建 Package

```
POST /api/packages/{name}/build
```

**说明**: 调用 `agentflow` 构建命令将 Package 源码目录打包为 `.tar.gz` 分发包。构建产物输出到 `<package>/dist/<name>-<sha256>.tar.gz`。支持同一 Package 的多个版本（不同 sha256）。

| 状态码 | 条件 |
|--------|------|
| 200 | 构建成功，返回构建结果（含 tarball 路径、SHA256、文件大小等） |
| 404 | Package 不存在 |
| 500 | 构建进程失败或超时（超时 60s） |

**响应示例**：

```json
{
  "success": true,
  "data": {
    "tarball": "my-agent-d4e5f6a7b8c9.tar.gz",
    "sha256": "d4e5f6a7b8c9..."
  },
  "error": null
}
```

---

### 2.14 获取 Package 构建产物信息

```
GET /api/packages/{name}
```

**说明**: 返回 Package 信息及已构建的分发包列表。

| 状态码 | 条件 |
|--------|------|
| 200 | 成功，返回 `PackageInfo` |
| 404 | Package 不存在 |

---

### 2.15 下载构建产物

```
GET /api/packages/{name}/download/{tarball}
```

**说明**: 下载指定版本的构建产物 `.tar.gz` 文件。

| 状态码 | 条件 |
|--------|------|
| 200 | 返回 `application/gzip` 二进制流 |
| 404 | Package 或 tarball 不存在 |

---

## 3. 端点汇总

### 元数据

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/packages` | 列出所有 Package |
| GET | `/api/packages/{name}` | 获取单个 Package 信息 |

### 文件浏览与编辑

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/packages/{name}/tree` | 递归文件树 |
| GET | `/api/packages/{name}/file/{path}` | 读取文件内容 |
| PUT | `/api/packages/{name}/file/{path}` | 写入/覆盖文件 |
| POST | `/api/packages/{name}/file/{path}` | 重命名（有 body）/ 创建空文件（无 body） |
| DELETE | `/api/packages/{name}/file/{path}` | 删除文件 |

### 目录操作

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/packages/{name}/dir` | 创建目录 |
| DELETE | `/api/packages/{name}/dir?path=X` | 删除目录（递归） |

### 包操作

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | `/api/packages/create` | 创建空 Package |
| POST | `/api/packages/upload` | 上传归档解压 |
| POST | `/api/packages/{name}/build` | 构建分发包 |
| GET | `/api/packages/{name}/download/{tarball}` | 下载构建产物 |

---

**权威章节映射**：

| 概念 | 权威章节 |
|------|----------|
| 端点定义 | 本文档 §2 / §3 |
| PackageInfo 模型 | 本文档 §1.3 |
| 安全校验规则 | 本文档 §1.2 |
| 上传格式规范 | 本文档 §2.12 |
| 构建流程 | 本文档 §2.13 |
| 功能设计 | `features/package-management.md` L2 |
| Pydantic 模型定义 | `frontend/bridge/models.py` |
| 源码实现 | `frontend/bridge/routes/packages.py` |

**相关文档**:
- `docs/design/features/package-management.md` — Package 管理功能设计（L2）
- `docs/design/features/bridge-overview.md` — Bridge 总体架构
- `docs/design/architecture/modules/daemon-client-protocol.md` — Daemon 通信协议（非本模块依赖，仅供架构参考）
