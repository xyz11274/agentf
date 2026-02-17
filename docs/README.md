# 文档指南

欢迎来到 AgentFlow 文档！所有项目文档已整合到此 `docs/` 目录，方便查阅和维护。

## 📂 根目录文档

项目根目录保留最关键的三个文件：
- **[README.md](../README.md)** - 项目主导航和快速入门
- **[CONTRIBUTING.md](../CONTRIBUTING.md)** - 贡献指南（GitHub标准文件）
- **[TODO.md](../TODO.md)** - 项目规划和功能状态

**建议首先阅读 [根目录 README.md](../README.md)，它包含了所有文档导航。**

---

## 📚 docs/ 目录文档列表

### 📖 核心文档

| 文档 | 用途 | 大小 |
|------|------|-----|
| **architecture.md** | 系统5视图架构设计 | 83KB |
| **planning.md** | 功能规划和优先级 | 10KB |
| **code-review.md** | 代码完备性评估和优化指南（P0/P1 已完成✅） | 17KB |
| **api-reference.md** | API 参考手册 | 30KB |
| **development.md** | 开发环境设置和规范 | 11KB |
| **dependencies.md** | 外部依赖管理 | 11KB |
| **testing.md** | 测试指南和方法 | 12KB |
| **roadmap.md** | 项目路线图 | 6.3KB |

### 📝 贡献和 CI/CD

| 文档 | 用途 | 大小 |
|------|------|-----|
| **contributing.md** | 贡献指南完整版 | 9.5KB |
| **ci-cd.md** | CI/CD 自动化管线（合并版） | 8.0KB |

**总文档量**：~200KB

---

## 🎯 快速导航

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

1. 查看 [planning.md](planning.md) - 功能实现状态
2. 查看 [code-review.md](code-review.md) - 代码现状和优化
3. 查看 [../TODO.md](../TODO.md) - 快速规划导航
4. 查看 [roadmap.md](roadmap.md) - 长期计划

### CI/CD 和部署

1. 查看 [ci-cd.md](ci-cd.md) - CI/CD 完整指南
2. 查看 [../tests/README_PERFORMANCE_TESTS.md](../tests/README_PERFORMANCE_TESTS.md) - 性能测试指南

### 运维和系统

1. 查看 [dependencies.md](dependencies.md) - 依赖管理
2. 查看 [../scripts/README.md](../scripts/README.md) - 脚本使用说明

---

## 🔗 按用途查找

| 我想... | 查看文档 |
|--------|--------|
| 学习系统架构 | [architecture.md](architecture.md) |
| 了解代码现状和优化点 | [code-review.md](code-review.md) |
| 查看 API 使用方法 | [api-reference.md](api-reference.md) |
| 开始开发 | [development.md](development.md) |
| 贡献代码 | [contributing.md](contributing.md) |
| 了解项目规划 | [planning.md](planning.md) |
| 查询 CI/CD | [ci-cd.md](ci-cd.md) |
| 了解依赖库 | [dependencies.md](dependencies.md) |
| 运行测试 | [testing.md](testing.md) |
| 使用脚本 | [../scripts/README.md](../scripts/README.md) |
| 查看路线图 | [roadmap.md](roadmap.md) |
| 性能测试和基准 | [../tests/README_PERFORMANCE_TESTS.md](../tests/README_PERFORMANCE_TESTS.md) |

---

## 📊 文档内容梗概

### architecture.md (83KB)
- 5视图架构（场景、逻辑、开发、运行、部署）
- 模块设计详解
- 设计模式说明

### api-reference.md (30KB)
- 核心 API 使用指南
- 代码示例
- 最佳实践

### code-review.md (17KB)
- 代码完备性评估
- P0 缺陷修复 ✅ 已完成
- P1 功能补全 ✅ 已完成
- P2 性能优化计划

### development.md (11KB)
- 环境配置步骤
- 编程规范
- 开发工作流程
- 工具和命令

### contributing.md (9.5KB)
- 代码规范
- 提交流程
- Pull Request 指南
- 行为守则

### testing.md (12KB)
- 测试方法
- 覆盖率要求
- 测试工具说明

### planning.md (10KB)
- P0-P4 优先级功能
- 各模块实现状态  
- 更新日志

### ci-cd.md (8KB) - 合并版
- 构建/测试/发布工作流
- 代码质量检查
- 本地开发指南
- 快速参考

### dependencies.md (11KB)
- 依赖列表
- 安装方式
- 隔离策略

### roadmap.md (6.3KB)
- 版本计划
- 特性愿景

---

## 📝 维护指南

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
- 例如：`api-reference.md`, `code-review.md`, `ci-cd.md`
- 例外：`README.md`（GitHub 标准）

### 文档标准
- 使用 Markdown 格式
- 清晰的目录结构
- 包含代码示例
- 相关文档间的链接

---

## 💡 获取帮助

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
- 2026-02-18: 合并 CI/CD 文档，统一命名规范为 lowercase-hyphen
- 2026-02-17: 添加 API 参考和代码审查文档
