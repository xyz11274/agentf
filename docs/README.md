# 文档指南

欢迎来到 AgentFlow 文档！所有项目文档已整合到此 `docs/` 目录，方便查阅和维护。

## 📂 根目录文档

项目根目录保留最关键的三个文件：
- **[README.md](../README.md)** - 项目主导航和快速入门
- **[CONTRIBUTING.md](../CONTRIBUTING.md)** - 贡献指南（GitHub标准文件）
- **[TODO.md](../TODO.md)** - 项目规划和功能状态

**建议首先阅读 [根目录 README.md](../README.md)，它包含了所有文档导航。**

---

## 📚 docs/ 目录完整列表

### 📖 核心文档

| 文档 | 用途 | 大小 |
|------|------|-----|
| **ARCHITECTURE.md** | 系统5视图架构设计 | 84KB |
| **PLANNING.md** | 功能规划和优先级 | 16KB |
| **CODE_REVIEW.md** | 代码完备性评估和优化指南 | 12KB |
| **development.md** | 开发环境设置和规范 | 12KB |
| **dependencies.md** | 外部依赖管理 | 12KB |
| **testing.md** | 测试指南和方法 | 13KB |
| **roadmap.md** | 项目路线图 | 6.1KB |

### 📝 贡献和参考

| 文档 | 用途 | 大小 |
|------|------|-----|
| **CONTRIBUTING.md** | 贡献指南完整版 | 9.5KB |
| **QUICK_REFERENCE.md** | 快速参考卡 | 3.2KB |

### ⚙️ CI/CD 和部署

| 文档 | 用途 | 大小 |
|------|------|-----|
| **CI_CD_PIPELINE.md** | 流水线自动化指南 | 11KB |
| **CI_CD_SETUP.md** | 配置和常见问题 | 8.7KB |
| **CI_CD_IMPLEMENTATION.md** | 实现细节 | 14KB |

### 🚀 性能测试 (新增)

| 文档 | 用途 | 位置 |
|------|------|-----|
| **README_PERFORMANCE_TESTS.md** | 性能测试框架完整指南 | ../tests/ |

**总文档量**：~230KB

---

## 🎯 快速导航

### 新用户入门

1. 查看 [../README.md](../README.md) - 项目概览和导航
2. 阅读 [ARCHITECTURE.md](ARCHITECTURE.md) - 了解系统架构
3. 查看 [development.md](development.md) - 设置开发环境
4. 参考 [QUICK_REFERENCE.md](QUICK_REFERENCE.md) - 常用命令

### 贡献代码前

1. 阅读 [CONTRIBUTING.md](CONTRIBUTING.md) - 完整贡献指南
2. 查看 [testing.md](testing.md) - 测试要求
3. 检查 [QUICK_REFERENCE.md](QUICK_REFERENCE.md) - 提交清单

### 了解项目规划

1. 查看 [PLANNING.md](PLANNING.md) - 功能实现状态
2. 查看 [../TODO.md](../TODO.md) - 快速规划导航
3. 查看 [roadmap.md](roadmap.md) - 长期计划

### CI/CD 和部署

1. 查看 [CI_CD_PIPELINE.md](CI_CD_PIPELINE.md) - 流水线完整文档
2. 查看 [CI_CD_SETUP.md](CI_CD_SETUP.md) - 配置和问题排查
3. 查看 [CI_CD_IMPLEMENTATION.md](CI_CD_IMPLEMENTATION.md) - 实现细节
4. 查看 [../tests/README_PERFORMANCE_TESTS.md](../tests/README_PERFORMANCE_TESTS.md) - 性能测试指南

### 运维和系统

1. 查看 [dependencies.md](dependencies.md) - 依赖管理
2. 查看 [../scripts/README.md](../scripts/README.md) - 脚本使用说明

---

## 🔗 按用途查找

| 我想... | 查看文档 |
|--------|--------|
| 学习系统架构 | [ARCHITECTURE.md](ARCHITECTURE.md) |
| 了解代码现状和优化点 | [CODE_REVIEW.md](CODE_REVIEW.md) |
| 开始开发 | [development.md](development.md) |
| 贡献代码 | [CONTRIBUTING.md](CONTRIBUTING.md) |
| 了解项目规划 | [PLANNING.md](PLANNING.md) |
| 查询 CI/CD | [CI_CD_PIPELINE.md](CI_CD_PIPELINE.md) |
| 快速查阅命令 | [QUICK_REFERENCE.md](QUICK_REFERENCE.md) |
| 了解依赖库 | [dependencies.md](dependencies.md) |
| 运行测试 | [testing.md](testing.md) |
| 使用脚本 | [../scripts/README.md](../scripts/README.md) |
| 查看路线图 | [roadmap.md](roadmap.md) |
| 性能测试和基准 | [../tests/README_PERFORMANCE_TESTS.md](../tests/README_PERFORMANCE_TESTS.md) |

---

## 📊 文档内容梗概

### ARCHITECTURE.md (84KB)
- 5视图架构（场景、逻辑、开发、运行、部署）
- 模块设计详解
- 设计模式说明

### development.md (12KB)
- 环境配置步骤
- 编程规范
- 开发工作流程
- 工具和命令

### CONTRIBUTING.md (9.5KB)
- 代码规范
- 提交流程
- Pull Request 指南
- 行为守则

### testing.md (13KB)
- 测试方法
- 覆盖率要求
- 测试工具说明

### PLANNING.md (16KB)
- P0-P4 优先级功能
- 各模块实现状态  
- 更新日志

### CI/CD 系列 (~34KB)
- **CI_CD_PIPELINE.md** - 流水线描述
- **CI_CD_SETUP.md** - 配置指南
- **CI_CD_IMPLEMENTATION.md** - 实现细节

### dependencies.md (12KB)
- 依赖列表
- 安装方式
- 隔离策略

### QUICK_REFERENCE.md (3.2KB)
- 常用命令
- Git 工作流
- 检查清单

### roadmap.md (6.1KB)
- 版本计划
- 特性愿景

### README_PERFORMANCE_TESTS.md (位于 ../tests/)
- 性能测试框架体系
- 5个测试场景详解
- 性能指标和基线
- 本地测试和调试方法
- CI/CD性能测试工作流

---

## 📝 维护指南

### 添加新文档
1. 在 `docs/` 目录创建 Markdown 文件
2. 编写清晰的标题和目录
3. 在本文件中更新文档列表
4. 在 [../README.md](../README.md) 中添加导航

### 更新现有文档
1. 保持与代码同步
2. 更新相关链接
3. 在文档尾部记录更新日期

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
- 参考 [CONTRIBUTING.md](CONTRIBUTING.md)

**需要快速参考？**
- 查看 [QUICK_REFERENCE.md](QUICK_REFERENCE.md)

---

**最后更新**：2026 年 2 月 17 日  
**维护者**：@DanielKernel
