# Skill Hub API 接口契约

> **层级**: L4 · `architecture/modules/skill-hub-api.md`
> **状态**: 已填充
> **创建日期**: 2026-06-25

---

## 1. What

本文档定义 Skill Hub（技能中心仓库）模块的完整 API 接口契约，包含所有端点的路径、请求/响应体 Schema、数据模型定义、错误码和安全校验规则。

本模块直接操作文件系统（`resource/skill_hub/` 目录），不经过 daemon。

---

## 2. 全局约定

### 2.1 响应信封

本模块端点**直接返回数据模型**（不经过 `{success, data, error}` 信封），
与 Bridge 的 Agent Package 管理模块（`routes/packages.py`）采用相同模式。

成功时直接返回模型 JSON，失败时返回 FastAPI 标准错误（`{"detail": "..."}`）。

### 2.2 数据模型

#### SkillInfo（列表元数据）

```json
{
  "name": "my-skill",
  "description": "技能的简短描述",
  "tags": ["tag1", "tag2"],
  "version": "1.0.0",
  "file_count": 2,
  "last_modified": "2026-06-25T15:00:00+08:00"
}
```

| 字段 | 类型 | 必填 | 可见性 | 来源 | 说明 |
|------|------|------|--------|------|------|
| `name` | string | 是 | 对外 | 目录名 | 技能唯一标识，`[a-zA-Z0-9_-]+` |
| `description` | string | 是 | 对外 | frontmatter | 简短描述，列表页卡片展示 |
| `tags` | string[] | 否 | 对外 | frontmatter | 标签列表，空数组时输出 `[]` |
| `version` | string | 否 | 对外 | frontmatter | 默认 `"1.0.0"` |
| `file_count` | int | 是 | 对外 | 文件系统 | 递归统计的非目录文件数 |
| `last_modified` | string | 是 | 对外 | 文件系统 | ISO 8601 格式，UTC |
| `name` 长度 | int | — | — | — | 1-128 字符 |

#### SkillDetail（详情页）

```json
{
  "name": "my-skill",
  "description": "技能的简短描述",
  "tags": ["tag1"],
  "version": "1.0.0",
  "content": "---\ndescription: 技能的简短描述\ntags: [tag1]\n---\n\n# 技能标题\n\n正文内容",
  "file_count": 2,
  "last_modified": "2026-06-25T15:00:00+08:00"
}
```

| 字段 | 类型 | 必填 | 可见性 | 说明 |
|------|------|------|--------|------|
| `content` | string | 是 | 对外 | SKILL.md 的完整文本内容（含 frontmatter） |

#### SkillInstallResponse（安装结果）

```json
{
  "name": "my-skill",
  "target_package": "harness-agent",
  "target_path": "/home/user/AgentFlow/examples/agent/harness-agent/harness/skills/my-skill",
  "file_count": 2
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `name` | string | 技能名 |
| `target_package` | string | 目标 Agent Package 名称 |
| `target_path` | string | 安装后的绝对路径 |
| `file_count` | int | 复制的文件数 |

### 2.3 字段层级示意

以下列出各字段在系统中的生命周期归属：

| 字段 | 运行时内部对象 | 前端展示 | API 响应输出 | 持久化存储 |
|------|---------------|----------|-------------|-----------|
| `name` | — | ✅ | ✅ | ✅（目录名） |
| `description` | — | ✅ | ✅ | ✅（SKILL.md frontmatter） |
| `tags` | — | ✅ | ✅ | ✅（SKILL.md frontmatter） |
| `version` | — | ✅ | ✅ | ✅（SKILL.md frontmatter） |
| `file_count` | — | ✅ | ✅ | ❌（运行时统计） |
| `last_modified` | — | ✅ | ✅ | ❌（运行时 mtime） |
| `content` | — | ✅ | ✅ | ✅（SKILL.md 全文） |
| `target_package` | — | ✅ | ✅ | ❌（仅安装时返回） |
| `target_path` | — | ✅ | ✅ | ❌（仅安装时返回） |

### 2.4 安全校验规则

**技能名校验**（`_validate_skill_name()`，逻辑同 `_validate_package_name()`）：

| 规则 | 值 |
|------|-----|
| 允许字符 | `[a-zA-Z0-9_-]` |
| 最小长度 | 1 |
| 最大长度 | 128 |
| 拒绝样例 | `"../etc"`、`"my skill"`、`"a/b"`、`""` |

**路径边界校验**（复用 `_resolve_within()`）：

```
输入 path → Posix 分段 → 拒绝 ".." 和绝对路径
→ Path(skill_hub_path / *segments).resolve()
→ 校验 resolved.parent == skill_hub_path
→ 越界 → 403 Forbidden
```

**安装目标校验**：

```
输入 target_package → _validate_package_name()
→ agent_packages_path / target_package → is_dir()
→ 不存在 → 404
→ 目标路径 = agent_packages_path / target_package / "harness" / "skills" / name
→ 已存在 → 409
→ 最终路径 _resolve_within() 确保不逃逸
```

### 2.5 后端存储路径配置

| 配置项 | 类型 | 默认值 | 环境变量 | 说明 |
|--------|------|--------|----------|------|
| `skill_hub_dir` | string | `"resource/skill_hub"` | `SKILL_HUB_DIR` | 技能中心仓库目录路径 |

**解析规则**（与 `agent_packages_dir` 一致）：

```python
@property
def skill_hub_path(self) -> Path:
    p = Path(self.skill_hub_dir)
    if p.is_absolute():
        return p.resolve()
    return (_PROJECT_ROOT / p).resolve()
```

### 2.6 HTTP 错误响应格式

本模块不经过 daemon，错误响应使用 FastAPI 标准格式：

```json
{
  "detail": "错误描述文本"
}
```

HTTP 状态码语义：

| 状态码 | 条件 |
|--------|------|
| 400 | 参数校验失败（文件格式、缺少字段） |
| 403 | 非法技能名或包名 |
| 404 | 技能或目标 Package 不存在 |
| 409 | 资源冲突（同名已存在且未允许覆盖） |
| 500 | 文件系统操作失败 |

错误响应示例：

```json
// 400 - 文件格式不支持
{"detail": "Unsupported file format: 'my-file.exe'. Allowed: .md, .zip, .tar.gz, .tgz"}

// 403 - 非法技能名
{"detail": "Invalid skill name: '../etc' (only [a-zA-Z0-9_-] allowed)"}

// 404 - 技能不存在
{"detail": "Skill 'my-skill' not found"}

// 409 - 已存在
{"detail": "Skill 'my-skill' already exists. Set overwrite=true to replace."}
```

---

## 3. API 端点

### 3.1 列出所有技能

```
GET /api/skill-hub
```

**说明**: 返回 `resource/skill_hub/` 下所有技能目录的元数据。遍历每个子目录读取 SKILL.md 的 YAML frontmatter。

**无分页**：当前技能数量预期在 100 以内，暂不实现分页。

**响应体**: `SkillInfo[]`（数组，无信封包装）

```json
[
  {
    "name": "my-skill",
    "description": "技能的简短描述",
    "tags": ["tag1"],
    "version": "1.0.0",
    "file_count": 2,
    "last_modified": "2026-06-25T15:00:00+08:00"
  }
]
```

| 状态码 | 条件 | 响应 data |
|--------|------|-----------|
| 200 | 成功 | `SkillInfo[]`（可能为空 `[]`） |

**边界情况**：
- 目录不存在 → 返回 `[]`
- 子目录下无 `SKILL.md` → 该目录不被视为有效技能，静默跳过
- 部分技能 frontmatter 解析失败 → 按默认值填充（`description: ""`, `tags: []`, `version: "1.0.0"`），**不**因此返回整体失败

**错误响应**：

| 条件 | HTTP | error |
|------|------|-------|
| 技能目录不可读（权限） | 500 | `"Failed to read skill hub directory: Permission denied"` |

---

### 3.2 获取技能详情

```
GET /api/skill-hub/{name}
```

**路径参数**:

| 参数 | 类型 | 约束 | 说明 |
|------|------|------|------|
| `name` | string | `[a-zA-Z0-9_-]{1,128}` | 技能名称 |

**响应体**: `SkillDetail`（无信封包装）

```json
{
  "name": "my-skill",
  "description": "技能的简短描述",
  "tags": ["tag1"],
  "version": "1.0.0",
  "content": "---\ndescription: 技能的简短描述\ntags: [tag1]\n---\n\n# 技能标题\n\n正文内容",
  "file_count": 2,
  "last_modified": "2026-06-25T15:00:00+08:00"
}
```

`content` 字段包含 SKILL.md 完整文本（含 frontmatter 和正文），由前端 MarkdownContent 组件渲染。

| 状态码 | 条件 |
|--------|------|
| 200 | 成功 |
| 403 | 非法技能名（`_validate_package_name` 失败） |
| 404 | 技能不存在（`skill_hub_path / name` 不是有效目录） |
| 500 | SKILL.md 读取失败（文件不存在或不可读） |

**错误响应示例**：

```json
{
  "success": false,
  "data": null,
  "error": "Skill 'my-skill' not found"
}
```

---

### 3.3 上传技能

```
POST /api/skill-hub/upload
```

**请求格式**: `multipart/form-data`

**请求字段**:

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `file` | UploadFile | 是 | — | 文件内容（.md / .zip / .tar.gz / .tgz） |
| `name` | string | 否 | 从文件名推断 | 技能名；省略时从文件名剥离扩展名 |
| `overwrite` | bool | 否 | `false` | 同名技能已存在时是否覆盖 |

**技能名推断规则**：

| 原始文件名 | 推断技能名 |
|-----------|-----------|
| `my-skill.md` | `my-skill` |
| `my-skill.zip` | `my-skill` |
| `my-skill.tar.gz` | `my-skill` |
| `my-skill.tgz` | `my-skill` |
| `my.skill.md` | `my.skill`（仅去掉最后一个 `.md` 后缀） |

**支持的归档格式**：

| 文件类型 | 处理方式 |
|----------|----------|
| `.md` 单文件 | 直接创建 `{name}/SKILL.md` |
| `.zip` | 解压到 `{name}/` |
| `.tar.gz` | 解压到 `{name}/` |
| `.tgz` | 同 `.tar.gz` |
| 其他后缀 | 400 Bad Request |

**校验规则**：

| 校验点 | 单文件 | 归档 |
|--------|--------|------|
| 文件后缀 | 必须是 `.md` | 必须是 `.zip` / `.tar.gz` / `.tgz` |
| 文件内容 | 必须以 `---\n` 开头（YAML frontmatter） | — |
| 解压后 | — | 根目录必须包含 `SKILL.md` |
| 技能名 | 同 §2.4 技能名校验 | 同左 |
| 目录冲突 | 已存在且 `overwrite=false` → 409 | 同左 |

**执行流程**：

```
1. 校验文件后缀（400 如果不支持）
2. 确定技能名（显式 name 或从文件名推断）
3. _validate_package_name()（403 如果非法）
4. 检查目录是否已存在（409 如果已存在且 !overwrite）
5. 保存上传文件到临时位置
6. 解压/创建技能目录
7. 校验 SKILL.md 存在（400 如果缺失）
8. 读取 frontmatter
9. 删除临时文件
10. 返回 SkillInfo
```

**覆盖写入流程**（`overwrite=true` 时）：

```
4'. 检查目录是否已存在
    └─ 存在 → 删除原目录（shutil.rmtree）
    └─ 不存在 → 正常创建
5-10. 同上
```

**响应体**: `SkillInfo`（无信封包装）

| 状态码 | 条件 |
|--------|------|
| 201 | 上传成功 |
| 400 | 文件格式不支持、无法解析 frontmatter、归档中缺少 SKILL.md |
| 403 | 非法技能名 |
| 409 | 同名技能已存在且 `overwrite=false` |
| 500 | 临时文件写入失败、解压失败 |

**错误响应示例**：

```json
{
  "success": false,
  "data": null,
  "error": "Unsupported archive format: 'my-file.exe'. Allowed: .md, .zip, .tar.gz, .tgz"
}
```

```json
{
  "success": false,
  "data": null,
  "error": "Archive does not contain SKILL.md at its root"
}
```

---

### 3.4 删除技能

```
DELETE /api/skill-hub/{name}
```

**路径参数**: `name` — 技能名称

**说明**: 递归删除技能目录（`shutil.rmtree`）。

| 状态码 | 条件 | 响应体 |
|--------|------|--------|
| 204 | 删除成功 | 无 |
| 403 | 非法技能名 | `{"detail": "..."}` |
| 404 | 技能不存在 | `{"detail": "..."}` |
| 500 | 文件系统删除失败 | `{"detail": "..."}` |

---

### 3.5 安装技能到 Agent Package

```
POST /api/skill-hub/{name}/install
```

**路径参数**: `name` — 技能名称

**请求体**:

```json
{
  "target_package": "harness-agent"
}
```

**`SkillInstallRequest` 字段**：

| 字段 | 类型 | 必填 | 约束 | 说明 |
|------|------|------|------|------|
| `target_package` | string | 是 | `[a-zA-Z0-9_-]{1,128}` | 目标 Agent Package 名称 |

**完整执行流程**：

```
1. _validate_package_name(name) → 非法则 403
2. _validate_package_name(target_package) → 非法则 403
3. 源路径 = skill_hub_path / name → 不存在 → 404
4. 目标包路径 = agent_packages_path / target_package → 不存在 → 404
5. 目标路径 = 目标包路径 / "harness" / "skills" / name
6. 目标已存在 → 409
7. 创建父目录（mkdir -p target的parent）
8. shutil.copytree(源路径, 目标路径)
9. file_count = sum(1 for _ in 目标路径.rglob("*") if _.is_file())
10. 返回安装结果
```

**响应体**: `SkillInstallResponse`（无信封包装）

```json
{
  "name": "my-skill",
  "target_package": "harness-agent",
  "target_path": "/home/user/AgentFlow/examples/agent/harness-agent/harness/skills/my-skill",
  "file_count": 2
}
```

| 状态码 | 条件 |
|--------|------|
| 200 | 安装成功 |
| 400 | 请求体缺失 `target_package` |
| 403 | 非法技能名或包名 |
| 404 | 技能或目标 Package 不存在 |
| 409 | 目标 Package 的 `harness/skills/{name}` 已存在 |
| 500 | 文件系统复制失败 |

**错误响应示例**：

```json
{
  "success": false,
  "data": null,
  "error": "Skill 'my-skill' is already installed in package 'harness-agent' at: .../harness/skills/my-skill"
}
```

---

## 4. 端点汇总

| 方法 | 路径 | 请求体 | 成功状态 | 失败状态 |
|------|------|--------|----------|----------|
| GET | `/api/skill-hub` | 无 | 200 | 500 |
| GET | `/api/skill-hub/{name}` | 无 | 200 | 403, 404, 500 |
| POST | `/api/skill-hub/upload` | `multipart/form-data` | 201 | 400, 403, 409, 500 |
| DELETE | `/api/skill-hub/{name}` | 无 | 200 | 403, 404, 500 |
| POST | `/api/skill-hub/{name}/install` | `{target_package}` | 200 | 400, 403, 404, 409, 500 |

---

## 5. 配置项

| 配置项 | 类型 | 环境变量 | 默认值 | 说明 |
|--------|------|----------|--------|------|
| `skill_hub_dir` | string | `SKILL_HUB_DIR` | `"resource/skill_hub"` | 中心仓库目录。相对路径锚定项目根目录 |

---

## 6. 前端对接说明

### 6.1 路由配置

```typescript
// web/src/router/index.ts
{
  path: 'skill-hub',
  name: 'skill-hub',
  component: SkillHubPage,
  meta: { title: '技能中心' },
},
{
  path: 'skill-hub/:name',
  name: 'skill-hub-detail',
  component: SkillDetailPage,
  meta: { title: '技能详情' },
},
```

### 6.2 导航菜单

```html
<!-- web/src/layouts/MainLayout.vue -->
<el-menu-item index="/skill-hub">
  <el-icon><Collection /></el-icon>
  <template #title>技能中心</template>
</el-menu-item>
```

### 6.3 类型定义（`types/agentflow.ts` 新增）

```typescript
/** A skill in the central hub repository. */
export interface SkillInfo {
  name: string;
  description: string;
  tags: string[];
  version: string;
  file_count: number;
  last_modified: string;
}

/** Full detail of a skill, including SKILL.md content. */
export interface SkillDetail extends SkillInfo {
  content: string;
}
```

### 6.4 API 调用封装

```typescript
// 通过 api/client.ts 复用
import { getApiClient } from '@/api/context'

// 列出技能
const skills = await getApiClient().get<SkillInfo[]>('/api/skill-hub')

// 技能详情
const detail = await getApiClient().get<SkillDetail>(`/api/skill-hub/${name}`)

// 上传（multipart 需直接用 fetchImpl）
const form = new FormData()
form.append('file', file)
if (name) form.append('name', name)
if (overwrite) form.append('overwrite', 'true')
const res = await getApiClient().fetchImpl('/api/skill-hub/upload', {
  method: 'POST',
  headers: { Accept: 'application/json' },
  body: form,
})

// 删除
await getApiClient().del(`/api/skill-hub/${name}`)

// 安装
await getApiClient().post(`/api/skill-hub/${name}/install`, {
  target_package: 'harness-agent',
})
```

---

## 7. 测试设计

### 7.1 测试矩阵

| 层级 | 测试点 | 关键验证 |
|------|--------|----------|
| Unit | `_validate_package_name` | 合法名通过，`..` 拒绝 |
| Unit | SKILL.md frontmatter 解析 | 完整 frontmatter、缺失字段、空 frontmatter |
| Unit | 技能名推断 | `.md`、`.zip`、`.tar.gz` 剥离正确 |
| Integration | GET /api/skill-hub | 空目录返回 `[]`，有技能返回正确元数据 |
| Integration | GET /api/skill-hub/{name} | 返回内容含 frontmatter + 正文 |
| Integration | POST upload (.md单文件) | 201 + 目录创建 + SKILL.md 内容正确 |
| Integration | POST upload (.zip归档) | 201 + 解压后文件完整 |
| Integration | POST upload 已存在且 !overwrite | 409 |
| Integration | POST upload 已存在且 overwrite=true | 200 覆盖成功 |
| Integration | DELETE | 目录删除，再次 GET 404 |
| Integration | POST install | 文件复制到目标 Package |
| Integration | POST install 目标已存在 | 409 |
| Integration | POST install 目标 Package 不存在 | 404 |
| E2E | 前端列表展示 | 技能卡片显示正确 |
| E2E | 前端上传流程 | 选文件 → 上传 → 列表刷新 |
| E2E | 前端安装流程 | 选择目标 → 安装 → 成功提示 |

### 7.2 关键不变量

| 不变量 | 验证方式 | 违反后果 |
|--------|----------|----------|
| 技能目录创建后 `SKILL.md` 始终存在 | 上传后和删除前校验 | 列表页静默跳过无效技能 |
| 安装不修改源技能目录 | 安装前后源目录 SHA 校验 | 源被修改则下次安装异常 |
| 删除操作严格限制在 `skill_hub_path` 内 | `_resolve_within()` 守卫 | 路径穿越 → 403 |
| 安装目标不逃逸 `agent_packages_path` | `_resolve_within()` 守卫 | 路径穿越 → 403 |

### 7.3 失败路径测试

| 失败路径 | 预期行为 |
|----------|----------|
| 上传空文件 | 400（SKILL.md 无 frontmatter） |
| 上传超大文件（>100MB） | 受 FastAPI 内存限制，可能 500 |
| 技能目录权限不可读 | 列表静默跳过，详情 500 |
| 安装时文件系统只读 | 500 + 错误描述 |
| 并发安装同一技能到同一 Package | 无锁，后到的先检查通过后可能冲突 |

---

**权威章节映射**：

| 概念 | 权威章节 |
|------|----------|
| 端点定义与参数 | 本文档 §3 |
| SkillInfo / SkillDetail 模型 | 本文档 §2.2 |
| 安全校验规则 | 本文档 §2.4 |
| 字段层级与可见性 | 本文档 §2.3 |
| 上传格式规范与校验 | 本文档 §3.3 |
| 安装执行流程 | 本文档 §3.5 |
| 错误响应格式 | 本文档 §3（各端点错误示例） |
| 配置项定义 | 本文档 §5 |
| 前端对接 | 本文档 §6 |
| 测试设计 | 本文档 §7 |
| 功能设计（L2） | `features/skill-hub.md` |
| 产品需求（L1） | `requirements/prd/skill-hub-prd.md` |

**相关文档**:
- `docs/design/features/skill-hub.md` — Skill Hub 功能设计（L2）
- `docs/design/features/bridge-overview.md` — Bridge 总体架构
- `docs/design/features/package-management.md` — Package 管理（复用安全校验）
- `docs/requirements/prd/skill-hub-prd.md` — 产品需求（L1）
- `frontend/bridge/routes/packages.py` — 复用 `_validate_package_name()` / `_resolve_within()`
