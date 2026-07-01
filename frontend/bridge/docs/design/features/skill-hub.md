# Skill Hub 功能设计

> **层级**: L2 · `docs/design/features/skill-hub.md`
> **状态**: 已填充
> **创建日期**: 2026-06-25

---

## 1. What

Skill Hub（技能中心仓库）是一个独立的 Web 功能模块，提供一个集中式的技能存储、浏览和分发能力。它在 Bridge 侧以 `resource/skill_hub/` 目录作为持久化存储，在 Vue 前端以独立的「技能中心」Tab 页呈现。

**技能格式**：遵循 Claude Code 生态的 SKILL.md 格式（YAML frontmatter + Markdown 正文）。每个技能是一个包含 `SKILL.md` 的目录，可选附带 `references/` 等辅助文件。

## 2. Why

| 动机 | 说明 |
|------|------|
| 技能发现 | 用户通过统一 Web 界面浏览所有可用技能 |
| 技能安装 | 从中心仓库一键安装到目标 Agent Package 的 `harness/skills/` |
| 集中管理 | 创建、上传、查看、删除在统一界面完成 |
| 独立生命周期 | 技能可独立维护版本和说明，不绑定 Agent Package |

## 3. How

### 3.1 逻辑视图（职责分解）

```
┌────────────────────────────────────────────────────────────────┐
│ Skill Hub 模块                                                   │
│                                                                │
│  ┌────────────────────┐   ┌─────────────────────────────────┐  │
│  │  前端表现层          │   │  后端服务层                      │  │
│  │                     │   │                                 │  │
│  │ SkillHubPage.vue    │   │ routes/skill_hub.py             │  │
│  │  - 技能列表（卡片）    │   │  - GET /api/skill-hub          │  │
│  │  - 上传对话框        │   │  - GET /api/skill-hub/{name}   │  │
│  │                     │   │  - POST /api/skill-hub/upload   │  │
│  │ SkillDetailPage.vue │   │  - DELETE /api/skill-hub/{name} │  │
│  │  - 技能详情渲染       │   │  - POST /api/skill-hub/...    │  │
│  │  - 安装对话框        │   │         {name}/install         │  │
│  │                     │   │                                 │  │
│  │ skillHubStore.ts    │   │  → 复用 packages.py 的路径校验   │  │
│  │  - 列表状态管理       │   │  → 直接操作文件系统              │  │
│  │  - API 调用封装      │   │                                 │  │
│  └────────────────────┘   └──────────────┬──────────────────┘  │
│                                          │                     │
│                                          ▼                     │
│                            ┌────────────────────────┐         │
│                            │ resource/skill_hub/     │         │
│                            │ （持久化存储层）         │         │
│                            │ 文件系统目录             │         │
│                            └────────────────────────┘         │
└────────────────────────────────────────────────────────────────┘
```

**模块职责**：

| 职责 | 归属 | 边界 |
|------|------|------|
| 技能元数据浏览 | 后端读取 SKILL.md frontmatter → 前端展示 | 不缓存，每次读取文件系统 |
| 技能全文查看 | 后端读取 SKILL.md 全文 → 前端 Markdown 渲染 | 不进 daemon |
| 技能上传与校验 | 后端接收文件 → 校验格式 → 写入文件系统 | 格式仅校验 frontmatter 基本结构 |
| 技能安装 | 后端跨目录复制技能文件到目标 Package | 校验来源和目标的合法性 |
| 技能删除 | 后端递归删除技能目录 | 限制在 `skill_hub_path` 内 |

### 3.2 开发视图（代码组织）

```
frontend/bridge/
├── routes/
│   └── skill_hub.py         # 新增：技能 CRUD + 安装 REST 路由
├── config.py                # 新增：skill_hub_dir 配置项

frontend/web/src/
├── pages/
│   ├── SkillHubPage.vue     # 新增：技能列表页
│   └── SkillDetailPage.vue  # 新增：技能详情页
├── stores/
│   └── skillHubStore.ts     # 新增：Pinia store
├── router/
│   └── index.ts             # 新增：/skill-hub 路由
├── layouts/
│   └── MainLayout.vue       # 新增：侧边栏菜单项
├── types/
│   └── agentflow.ts         # 新增：SkillInfo / SkillDetail 接口
└── api/
    └── client.ts            # 复用（无需改动）
```

**依赖关系**：

```
routes/skill_hub.py
  → config.py (settings.skill_hub_path / settings.agent_packages_path)
  → models.py (PackageInfo 模型)
  → shutil / os 标准库（文件操作）

skillHubStore.ts
  → api/client.ts (getApiClient)
  → types/agentflow.ts (SkillInfo / SkillDetail)

SkillHubPage.vue / SkillDetailPage.vue
  → skillHubStore.ts
  → components/MarkdownContent.vue（复用 Markdown 渲染）
```

### 3.3 进程视图（关键时序）

#### 技能上传时序

```
前端                          Bridge                         文件系统
  │                            │                                │
  │ POST /api/skill-hub/upload │                                │
  │ (multipart: file + name)   │                                │
  │───────────────────────────→│                                │
  │                            │ 1. 校验文件后缀                  │
  │                            │    └─ .md/.zip/.tar.gz/.tgz    │
  │                            │ 2. 确定技能名                   │
  │                            │    └─ 显式 name 或从文件名推断   │
  │                            │ 3. 校验技能名合法性              │
  │                            │    └─ _validate_package_name() │
  │                            │ 4. 检查是否已存在               │
  │                            │    └─ 存在且 !overwrite → 409  │
  │                            │ 5. 保存临时文件                 │
  │                            │── tmpfile ← content ──────────│
  │                            │ 6. 创建技能目录 / 解压           │
  │                            │── mkdir .md 单文件 →           │
  │                            │   写入 {name}/SKILL.md          │
  │                            │── 或 解压归档到 {name}/ ──────→│
  │                            │ 7. 校验 SKILL.md 存在           │
  │                            │ 8. 读取 frontmatter             │
  │                            │ 9. 删除临时文件                 │
  │                            │ 10. 返回 SkillInfo              │
  │←── 201 {name, description, tags, file_count}              │
```

#### 技能安装时序

```
前端                          Bridge                         文件系统
  │                            │                                │
  │ POST /api/skill-hub/{name}/│                                │
  │   install                  │                                │
  │   {target_package}         │                                │
  │───────────────────────────→│                                │
  │                            │ 1. 验证来源：skill_hub/{name}   │
  │                            │    └─ 不存在 → 404              │
  │                            │ 2. 验证目标：agent_packages     │
  │                            │    └─ 不存在 → 404              │
  │                            │ 3. 构建目标路径                  │
  │                            │    └─ {dest}/harness/skills/    │
  │                            │       {name}/                   │
  │                            │ 4. 检查目标是否已存在            │
  │                            │    └─ 存在 → 409                │
  │                            │ 5. 创建父目录 harness/skills/   │
  │                            │── mkdir -p ──────────────────→│
  │                            │ 6. 递归复制技能目录              │
  │                            │── copytree ──────────────────→│
  │                            │ 7. 返回安装结果                  │
  │←── 200 {name, target_      │                                │
  │    package, target_path}  │                                │
```

### 3.4 技能目录结构

```
resource/skill_hub/
├── my-skill/                  ← 技能名 = 目录名
│   ├── SKILL.md               ← 技能定义（YAML frontmatter + Markdown 正文）
│   └── references/            ← 可选：参考资料目录
│       └── example.md
└── another-skill/
    └── SKILL.md
```

**SKILL.md 格式**：

```markdown
---
description: 技能的简短描述
tags: [tag1, tag2]
version: 1.0.0
---

# 技能标题

技能正文内容...
```

**frontmatter 字段语义**：

| 字段 | 类型 | 必填 | 默认值 | 可见范围 | 说明 |
|------|------|------|--------|----------|------|
| `description` | string | 是 | — | 对外（列表 + 详情页） | 简短描述，卡片展示使用 |
| `tags` | string[] | 否 | `[]` | 对外（列表 + 详情页） | 标签分类 |
| `version` | string | 否 | `1.0.0` | 对外（详情页） | 语义化版本号 |

### 3.5 文件操作安全模型

技能仓库仅接受目录名（不含路径分隔符操作），因此安全校验简化为两层：

| 层 | 函数 | 校验内容 | 拒绝条件 |
|----|------|----------|----------|
| 1 技能名 | `_validate_skill_name()` | `[a-zA-Z0-9_-]{1,128}` | `..`、`/`、空格、空字符串、超长 |
| 2 目录边界 | `settings.skill_hub_path / name` | resolve 后 parent 校验 | 越界 |

技能名校验逻辑与 Package 管理的 `_validate_package_name()` 等效。
由于技能模块不操作任意文件路径（只有技能名 → 目录映射），无需 `_validate_relative_path()` 这类相对路径校验。

安装目标校验额外检查：

| 校验 | 方法 | 违反后果 |
|------|------|----------|
| 目标 Package 存在 | `(agent_packages_path / target).is_dir()` | 404 |
| 目标 `harness/skills/` 不冲突 | `(dest / harness / skills / name).exists()` | 409 |

### 3.6 关键架构决策

| 决策 | 选择 | 理由 | 被拒绝方案 |
|------|------|------|-----------|
| 存储方式 | 文件系统目录 | 与现有 Package 管理一致，无额外依赖 | 数据库（增加运维复杂度） |
| 技能目录结构 | 每技能一目录 | 支持附带 `references/` 等辅助文件 | 单文件存储（无法扩展） |
| 校验范围 | 仅 frontmatter 格式 | 最小校验，不阻塞上传 | 深度语义校验（不阻塞可先不做） |
| 安装方式 | 文件复制（copytree） | 简单可靠，无需 daemon 参与 | 通过 daemon RPC（需额外开发） |
| 并发控制 | 无锁 | 低频操作，冲突概率低 | 文件锁（过度设计） |

### 3.7 前端组件设计

**路由**：

```
/skill-hub                    SkillHubPage.vue    — 技能列表（首页）
/skill-hub/:name              SkillDetailPage.vue — 技能详情 + 安装
```

**SkillHubPage 页面结构**：

```
┌─────────────────────────────────────────────┐
│ 技能中心                     [上传按钮]       │
│                                              │
│ ┌──────────────┐  ┌──────────────┐          │
│ │ 技能卡片       │  │ 技能卡片       │          │
│ │ 📄 my-skill   │  │ 📄 my-skill2  │          │
│ │ 简短描述...    │  │ 简短描述...    │          │
│ │ 🏷️ tag1 tag2  │  │ 🏷️ tag3       │          │
│ │ [查看] [安装]  │  │ [查看] [安装]  │          │
│ └──────────────┘  └──────────────┘          │
└─────────────────────────────────────────────┘
```

**SkillDetailPage 页面结构**：

```
┌─────────────────────────────────────────────┐
│ ← 技能中心     my-skill        [安装到Agent]  │
│                                              │
│ 描述：技能的简短描述                           │
│ 标签：🏷️ tag1  🏷️ tag2                      │
│ 版本：v1.0.0     文件数：3                     │
│ ───────────────────────────────────────────── │
│                                              │
│ # 技能标题                                    │
│                                              │
│ 技能正文内容（Markdown 渲染）                  │
└─────────────────────────────────────────────┘
```

**Pinia Store**（`useSkillHubStore`）：

| 状态 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `skills` | `SkillInfo[]` | `[]` | 技能列表 |
| `currentSkill` | `SkillDetail \| null` | `null` | 当前查看的技能详情 |
| `isLoading` | `boolean` | `false` | 加载中 |
| `error` | `string \| null` | `null` | 错误信息 |
| `installingName` | `string \| null` | `null` | 正在安装的技能名 |

| 动作 | 异步 | 说明 |
|------|------|------|
| `loadSkills()` | ✅ | 加载所有技能 |
| `loadSkillDetail(name)` | ✅ | 加载技能详情 |
| `uploadSkill(file, name?, overwrite?)` | ✅ | 上传技能 |
| `deleteSkill(name)` | ✅ | 删除技能 |
| `installSkill(name, targetPackage)` | ✅ | 安装技能到 Agent Package |

**前端类型定义**（`types/agentflow.ts` 新增）：

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

### 3.8 异常与错误处理

| 异常场景 | 错误类型 | HTTP | 响应 error 示例 |
|----------|----------|------|----------------|
| 非法技能名（含 `..` 等） | 参数错误 | 403 | `"Invalid skill name: 'my/../skill'"` |
| 技能目录已存在且未授权覆盖 | 资源冲突 | 409 | `"Skill 'my-skill' already exists"` |
| 上传文件格式不支持 | 参数错误 | 400 | `"Unsupported file format: .exe. Allowed: .md, .zip, .tar.gz, .tgz"` |
| 归档中缺少 SKILL.md | 参数错误 | 400 | `"Archive does not contain SKILL.md at root"` |
| 目标 Package 不存在 | 资源不存在 | 404 | `"Target package 'unknown_agent' not found"` |
| 目标 Package 技能冲突 | 资源冲突 | 409 | `"Skill 'my-skill' already installed in package 'harness-agent'"` |
| 文件系统写入失败 | 内部错误 | 500 | `"Failed to write file: Permission denied"` |

### 3.9 配置项

| 配置项 | 环境变量 | 默认值 | 解析规则 |
|--------|----------|--------|----------|
| `skill_hub_dir` | `SKILL_HUB_DIR` | `resource/skill_hub` | 相对路径锚定项目根目录，绝对路径直接使用 |

配置项属性 `skill_hub_path` 的解析逻辑（同 `agent_packages_path`）：

```
path = Path(self.skill_hub_dir)
if path.is_absolute():
    return path.resolve()
return (_PROJECT_ROOT / path).resolve()
```

### 3.10 不支持项 / 边界

| 场景 | 状态 | 说明 |
|------|------|------|
| 技能版本管理 | ❌ 不支持 | 同名技能覆盖写入，不保留历史 |
| 全文搜索 | ❌ 不支持 | 仅前端按名称/标签筛选 |
| 评分/评论/收藏 | ❌ 不支持 | 不在本阶段 |
| 在线编辑器 | ❌ 不支持 | 下载后编辑再上传 |
| 跨节点分发 | ❌ 不支持 | 仅管理本机文件系统 |
| 依赖管理 | ❌ 不支持 | 技能间依赖不在本模块处理 |
| 并发写入保护 | ❌ 不支持 | 无文件锁 |
| 大文件上传保护 | ❌ 不支持 | 无显式大小上限 |

---

**接口契约（端点路径、请求/响应 Schema、安装参数）请参见 `architecture/modules/skill-hub-api.md`**

**权威章节映射**：

| 概念 | 权威章节 |
|------|----------|
| 功能范围与不支持项 | 本文档 §1 / §3.10 |
| 技能目录结构 | 本文档 §3.4 |
| 前端组件设计 | 本文档 §3.7 |
| 异常与错误分类 | 本文档 §3.8 |
| 配置项定义 | 本文档 §3.9 |
| API 端点与 Schema | `architecture/modules/skill-hub-api.md` L4 |
| 产品范围 | `requirements/prd/skill-hub-prd.md` L1 |

**相关设计文档**:
- `docs/design/features/bridge-overview.md` — Bridge 总体架构
- `docs/design/features/package-management.md` — Package 管理（技能安装目标 + 复用安全校验）
- `docs/design/architecture/modules/skill-hub-api.md` — Skill Hub API 接口契约（L4）
- `docs/requirements/prd/skill-hub-prd.md` — 产品需求（L1）
