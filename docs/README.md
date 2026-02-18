# 文档指南

欢迎来到 AgentFlow 文档！所有项目文档已整合到此 `docs/` 目录，方便查阅和维护。

## 根目录文档

项目根目录保留最关键的三个文件：
- **[README.md](../README.md)** - 项目主导航和快速入门
- **[CONTRIBUTING.md](../CONTRIBUTING.md)** - 贡献指南（GitHub标准文件）
- **[TODO.md](../TODO.md)** - 项目规划和功能状态

**建议首先阅读 [根目录 README.md](../README.md)，它包含了所有文档导航。**

---

## docs/ 目录文档列表

### 核心文档

| 文档 | 用途 |
|------|------|
| **[architecture.md](architecture.md)** | 系统5视图架构设计 |
| **[planning.md](planning.md)** | 功能规划、路线图和代码完备性评估 |
| **[api-reference.md](api-reference.md)** | API 参考手册 |
| **[development.md](development.md)** | 开发环境设置、规范和依赖管理 |
| **[testing.md](testing.md)** | 测试指南和方法 |

### 贡献和 CI/CD

| 文档 | 用途 |
|------|------|
| **[contributing.md](contributing.md)** | 贡献指南完整版 |
| **[ci-cd.md](ci-cd.md)** | CI/CD 自动化管线 |

### 重构文档

| 文档 | 用途 |
|------|------|
| **[REFACTORING.md](REFACTORING.md)** | 代码重构记录和说明 |

---

## 快速导航

### 新用户入门

1. 查看 [../README.md](../README.md) - 项目概览和导航
2. 阅读 [architecture.md](architecture.md) - 了解系统架构
3. 查看 [api-reference.md](api-reference.md) - API 使用指南
4. 查看 [development.md](development.md) - 设置开发环境

### 贡献代码前

1. 阅读 [contributing.md](contributing.md) - 完整贡献指南
2. 查看 [testing.md](testing.md) - 测试要求
3. 查看 [ci-cd.md](ci-cd.md) - 本地质量检查

### 了解项目规划

1. 查看 [planning.md](planning.md) - 功能实现状态、路线图和代码评估
2. 查看 [../TODO.md](../TODO.md) - 快速规划导航

### CI/CD 和部署

1. 查看 [ci-cd.md](ci-cd.md) - CI/CD 完整指南

### 运维和系统

1. 查看 [development.md](development.md) 第8章 - 依赖管理
2. 查看 [planning.md](planning.md) - 构建和部署目标

---

## 按用途查找

| 我想... | 查看文档 |
|--------|--------|
| 学习系统架构 | [architecture.md](architecture.md) |
| 了解代码现状和优化点 | [planning.md](planning.md) - 代码完备性评估章节 |
| 查看 API 使用方法 | [api-reference.md](api-reference.md) |
| 开始开发 | [development.md](development.md) |
| 了解依赖库 | [development.md](development.md) - 第8章依赖管理 |
| 贡献代码 | [contributing.md](contributing.md) |
| 了解项目规划 | [planning.md](planning.md) |
| 查询 CI/CD | [ci-cd.md](ci-cd.md) |
| 运行测试 | [testing.md](testing.md) |
| 了解重构进展 | [REFACTORING.md](REFACTORING.md) |

---

## 文档内容梗概

### architecture.md
- 5视图架构（场景、逻辑、开发、运行、部署）
- 模块设计详解
- 设计模式说明

### planning.md（合并版）
- 功能实现状态概览
- 待实现功能（按优先级）
- 版本规划
- **代码完备性评估**（原 code-review.md 内容）
  - 文档与代码一致性审查
  - 代码 Bug 清单
  - 测试覆盖度评估
  - 代码优化建议
  - 优化实施路线
- 代码质量目标
- 文档目标
- 构建和部署目标

### api-reference.md
- 核心 API 使用指南
- 代码示例
- 最佳实践

### development.md（合并版）
- 环境配置步骤
- 编程规范
- 开发工作流程
- 组件概述
- 代码示例
- 工具配置
- **依赖管理**（原 dependencies.md 内容）
  - 依赖概览
  - 可选依赖详解
  - 依赖安装指南
  - 依赖隔离策略
  - Stub 实现说明
  - 故障排除

### contributing.md
- 代码规范
- 提交流程
- Pull Request 指南
- 行为守则

### testing.md
- 测试方法
- 覆盖率要求
- 测试工具说明

### ci-cd.md
- 构建/测试/发布工作流
- 代码质量检查
- 本地开发指南
- 快速参考

---

## 维护指南

### 添加新文档
1. 在 `docs/` 目录创建 Markdown 文件（使用 lowercase-hyphen 命名）
2. 编写清晰的标题和目录
3. 在本文件中更新文档列表
4. 在 [../README.md](../README.md) 中添加导航

### 更新现有文档
1. 保持与代码同步
2. 更新相关链接
3. 在文档尾部记录更新日期

### 文档命名规范
- 使用小写字母和连字符（lowercase-hyphen）
- 例如：`api-reference.md`, `ci-cd.md`
- 例外：`README.md`（GitHub 标准）, `REFACTORING.md`（历史保留）

### 文档标准
- 使用 Markdown 格式
- 清晰的目录结构
- 包含代码示例
- 相关文档间的链接

---

## 获取帮助

**找不到需要的信息？**
- 查看 [../README.md](../README.md) 的完整导航

**文档有错误或不清晰？**
- 提交 Issue 或 Pull Request
- 参考 [contributing.md](contributing.md)

**需要快速参考？**
- 查看 [ci-cd.md](ci-cd.md) 的快速参考部分

---

**最后更新**：2026 年 2 月 18 日
**维护者**：@DanielKernel
**变更记录**：
- 2026-02-18: 合并 roadmap.md 和 code-review.md 到 planning.md，合并 dependencies.md 到 development.md
- 2026-02-18: 合并 CI/CD 文档，统一命名规范为 lowercase-hyphen
- 2026-02-17: 添加 API 参考和代码审查文档
