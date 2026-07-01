# Skill Hub — 产品需求概述

> **层级**: L1 · `docs/requirements/prd/skill-hub-prd.md`
> **状态**: 已填充
> **创建日期**: 2026-06-25

---

## 1. What（解决什么问题）

Skill Hub（技能中心仓库）是一个集中管理 AgentFlow 生态中所有 SKILL.md 技能的 Web 功能模块。它解决以下问题：

| 问题 | 当前状态 | 目标状态 |
|------|----------|----------|
| 技能分散 | 各 Agent Package 的 `harness/skills/` 目录下各自存放 | 统一集中到中央仓库 |
| 技能复用难 | 用户无法浏览可用技能，也不了解其他 Agent 的技能集合 | 一目了然看到所有技能 |
| 管理零散 | 技能操作分散在 CLI 和 git 操作中 | 统一的 Web 界面完成 CRUD |
| 缺乏分发通道 | 跨 Agent 的技能复制需要手动操作 | 一键安装到目标 Package |

## 2. Why（为什么需要）

- **技能发现**: 用户需要浏览所有可用技能，而不是翻阅 Agent Package 的文件目录
- **快速安装**: 从中心仓库选择技能后，直接安装到目标 Agent Package 的 `harness/skills/` 下
- **集中管理**: 创建、上传、查看、删除在统一界面完成
- **独立生命周期**: 技能独立于 Agent Package，可独立维护版本和说明

## 3. 用户角色

| 角色 | 典型操作 | 使用频次 |
|------|----------|----------|
| Agent 开发者 | 上传新技能、删除废弃技能 | 中频（每周） |
| Agent 使用者 | 浏览技能、安装技能到自己的 Agent | 高频（每日） |
| 系统管理员 | 管理中心仓库的存储、清理 | 低频（每月） |

## 4. How（如何实现 — 概要）

Bridge 新增 `routes/skill_hub.py` 模块，提供 REST API 操作 `resource/skill_hub/` 目录。Vue 前端新增「技能中心」Tab 页。

| 能力 | 方式 | 端点 | 说明 |
|------|------|------|------|
| 技能列表 | HTTP GET | `/api/skill-hub` | 浏览中心仓库所有技能 |
| 技能详情 | HTTP GET | `/api/skill-hub/{name}` | 查看元数据和 SKILL.md 全文 |
| 技能上传 | HTTP POST | `/api/skill-hub/upload` | 单文件或归档上传 |
| 技能删除 | HTTP DELETE | `/api/skill-hub/{name}` | 从中心仓库删除 |
| 技能安装 | HTTP POST | `/api/skill-hub/{name}/install` | 安装到 Agent Package |

## 5. In Scope

- 技能中心仓库的 Web 界面（新 Tab + 详情页）
- 技能中心仓库的 REST API（Bridge 新增 `routes/skill_hub.py`）
- 技能文件在 `resource/skill_hub/` 下的存储、读取、上传、删除
- 从中心仓库安装技能到 Agent Package 的 `harness/skills/` 目录
- 技能的元数据浏览（名称、描述、标签、版本）
- SKILL.md 格式校验（YAML frontmatter 基本格式）

## 6. Out of Scope

| 能力 | 原因 |
|------|------|
| 技能版本管理 | 每个技能名只有最新版本，覆盖写入 |
| 跨节点自动分发 | 超出本模块范围（由 NFS/git 处理） |
| 技能市场/评分/评论 | 不在本阶段产品规划内 |
| SKILL.md 深度校验（frontmatter 字段完整性） | 不阻塞技能上传，暂不做语义层面校验 |
| 现 Agent Package 的技能加载机制 | 技能安装后仍由 daemon 加载 |
| 在线编辑器 | 仅支持上传后查看，编辑需下载后处理 |

## 7. 前置依赖与约束

| 依赖/约束 | 说明 | 影响 |
|-----------|------|------|
| Bridge 已就绪 | 依赖 FastAPI 运行环境和 `agent_packages_path` 配置 | 阻塞：Bridge 不运行则无法使用 |
| 技能格式 | 遵循 SKILL.md 标准（YAML frontmatter + Markdown） | 设计约束：上传技能必须符合此格式 |
| 文件系统权限 | Bridge 进程需对 `resource/skill_hub/` 有读写权限 | 部署配置 |
| 已有 Package 管理 | 技能安装时依赖 Package 管理的目录验证逻辑 | 可复用 `_validate_package_name()` |

## 8. 成功指标

| 指标 | 目标值 | 测量方法 |
|------|--------|----------|
| 技能列表加载 | < 500ms（100 技能时） | API 响应时间监控 |
| 技能上传（单 SKILL.md） | < 2s | API 响应时间监控 |
| 技能上传（归档 1MB） | < 5s | API 响应时间监控 |
| 技能安装到 Agent | < 1s（同机文件复制 10KB） | API 响应时间监控 |
| 中心仓库容量 | 支持 100+ 技能 | 文件系统压测 |
| 并发安装 | 无锁保护，串行安全 | 设计约束 |

## 9. 待确认规格

| 规格项 | 当前事实 | 需拍板 |
|--------|----------|--------|
| `skill_hub_dir` 默认值 | `resource/skill_hub`（相对路径） | 是否使用绝对路径或可配置 |
| 技能上传大小上限 | 无显式限制（受 FastAPI UploadFile 内存限制） | 是否需要设定最大上传大小 |
| 安装时同名冲突策略 | 返回 409（不覆盖） | 是否改为覆盖安装 |
| 并发控制 | 无文件锁 | 是否需要在文件操作加锁 |

---

**相关设计文档**:
- `docs/design/features/skill-hub.md` — Skill Hub 功能设计（L2）
- `docs/design/architecture/modules/skill-hub-api.md` — Skill Hub API 接口契约（L4）
- `docs/design/features/bridge-overview.md` — Bridge 总体架构
- `docs/design/features/package-management.md` — Package 管理（技能安装目标）
