# CI/CD 自动化管线

## 概述

AgentFlow 项目采用 GitHub Actions 实现完整的 CI/CD 自动化基础设施，支持 AMD64/ARM64 硬件平台和 Linux/macOS 操作系统。

## 支持平台

### 硬件架构
- **AMD64 (x86_64)**: Intel/AMD 64位处理器
- **ARM64 (aarch64)**: ARM 64位处理器 (Apple Silicon, AWS Graviton 等)

### 操作系统
- **Linux**: Ubuntu 24.04 LTS
  - AMD64: 原生 Ubuntu runners
  - ARM64: QEMU 用户空间模拟的交叉编译
- **macOS**: 多版本支持
  - AMD64: macOS 12+ (Intel 平台)
  - ARM64: macOS 14 (Apple Silicon - M1/M2/M3)

## 工作流文件

### 已创建的 Workflow

- ✅ `.github/workflows/build.yml` - 多平台构建管线
- ✅ `.github/workflows/code-quality.yml` - 代码质量与风格检查
- ✅ `.github/workflows/tests.yml` - 综合测试框架
- ✅ `.github/workflows/release.yml` - 自动化发布
- ✅ `.github/workflows/manual_build.yml` - 手动构建触发
- ✅ `.github/workflows/badge_status.yml` - 状态徽章生成

### 配置文件

- ✅ `.github/CODEOWNERS` - 代码所有权和审查分配
- ✅ `.github/dependabot.yml` - 自动化依赖更新
- ✅ `.github/pull_request_template.md` - PR 模板
- ✅ `.github/ISSUE_TEMPLATE/` - Issue 模板（bug、feature、performance）

---

## 1. Build Workflow (构建工作流)

**触发事件:**
- 推送到 `main` 或 `develop` 分支
- Pull Request 到 `main` 或 `develop` 分支

**矩阵配置:**
- Linux AMD64 (Ubuntu Latest)
- Linux ARM64 (Ubuntu Latest + QEMU)
- macOS AMD64 (macOS 12)
- macOS ARM64 (macOS 14)

**构建步骤:**
1. 检出源码
2. 安装平台依赖（OpenSSL, Protobuf, libwebsockets, libevent, RapidJSON, GTest）
3. 配置 CMake (Release 模式)
4. 并行构建 (`-j` 自动检测核心数)
5. 上传构建产物（7天保留）

**构建配置:**
```cmake
-DCMAKE_BUILD_TYPE=Release
-DCMAKE_CXX_STANDARD=17
-DAGENTFLOW_WITH_OPENSSL=ON
-DAGENTFLOW_WITH_PROTOBUF=ON
-DAGENTFLOW_WITH_LIBWEBSOCKETS=ON
```

**特性:**
- 并行构建，自动 CPU 检测
- 编译器版本报告
- 构建产物存储（7天保留）

---

## 2. Code Quality Workflow (代码质量工作流)

**触发事件:**
- 推送到 `main` 或 `develop` 分支
- Pull Request 到 `main` 或 `develop` 分支
- 手动触发（workflow_dispatch）

### 质量检查项

#### a. Clang Format 检查
- 验证代码格式一致性
- 使用 `.clang-format` 配置
- 报告需要格式化的文件
- 100% 通过要求

#### b. Clang Tidy 静态分析
- 代码质量问题检测
- 性能反模式识别
- 现代化建议
- 生成 `compile_commands.json` 用于分析

#### c. Cppcheck 静态分析
- 综合静态分析
- 内存泄漏检测
- 逻辑错误识别
- 可配置的严重性级别

#### d. Cpplint 风格检查
- Google C++ 风格指南强制执行
- 可自定义行长度（100字符）
- 版权和头文件验证

#### e. Sanitizer 编译检查
- AddressSanitizer (ASAN) 验证
- 提前检测内存安全问题

---

## 3. Test Workflow (测试工作流)

**触发事件:**
- 推送到 `main` 或 `develop` 分支
- Pull Request 到 `main` 或 `develop` 分支

### 测试类型

#### a. 单元测试
- 多平台测试执行
- GTest 框架集成
- 并行测试执行
- 60秒超时限制
- 详细失败报告

#### b. 代码覆盖率分析
- LCOV 覆盖率收集
- Gcovr 报告生成
- Codecov 集成
- HTML 覆盖率报告（30天保留）
- 自动 PR 注释

#### c. 集成测试
- 示例应用测试
- Workflow 验证
- 端到端功能检查

#### d. 内存泄漏检测
- Valgrind 工具集成
- 堆和栈泄漏检测
- 可配置的抑制规则

---

## 4. Release Workflow (发布工作流)

**触发事件:**
- Git tag 推送 (v* 模式，如 v1.0.0)

**自动化发布流程:**
1. 多平台二进制构建
2. 产物打包和压缩
3. SHA256 校验和生成
4. GitHub Release 页面创建
5. 资源发布

**分发包内容:**
- 头文件、库文件、CMake 文件
- 平台特定命名约定
- 完整文档

---

## 5. Manual Build Workflow (手动构建工作流)

**用户可配置参数:**
- 构建类型选择 (Debug/Release/RelWithDebInfo)
- 代码覆盖率启用/禁用
- Valgrind 内存检查
- Slack 通知

**使用方法:**
1. 进入 Actions 标签 → Manual Build & Test
2. 点击 "Run workflow"
3. 选择选项并运行

---

## 6. Status Badge Workflow (状态徽章工作流)

自动生成项目状态徽章，显示：
- 构建状态
- 测试状态
- 代码覆盖率
- 代码质量

---

## 本地开发快速参考

### 克隆与构建

```bash
# 克隆仓库
git clone https://github.com/DanielKernel/AgentFlow.git
cd AgentFlow

# 构建
./build.sh build

# 测试
./build.sh test

# 运行示例
./build.sh example hello_world
```

### 提交规范

```bash
# 格式: <type>(<scope>): <subject>
git commit -m "feat(actor): add message priority queue"

# 类型: feat, fix, docs, style, refactor, perf, test, chore
# 范围: framework, actor, dsl, event, network, storage, serialization, runtime, build, ci
```

### 推送前代码质量检查

```bash
# 格式检查和修复
clang-format --dry-run --Werror src/*.cpp          # 检查
clang-format -i -style=file src/*.cpp               # 修复

# 静态分析
clang-tidy -p build src/your_file.cpp

# 代码分析
cppcheck --enable=all src/

# 风格检查
cpplint src/your_file.cpp
```

### Git 工作流

```bash
# 创建功能分支
git checkout -b feature/my-feature

# 提交更改
git add .
git commit -m "feat(scope): description"

# 推送并创建 PR
git push origin feature/my-feature
```

---

## 状态徽章

在 README.md 中添加以下徽章：

```markdown
[![Build](https://github.com/DanielKernel/AgentFlow/workflows/Build/badge.svg?branch=main)](https://github.com/DanielKernel/AgentFlow/actions/workflows/build.yml)
[![Tests](https://github.com/DanielKernel/AgentFlow/workflows/Tests/badge.svg?branch=main)](https://github.com/DanielKernel/AgentFlow/actions/workflows/tests.yml)
[![Code Quality](https://github.com/DanielKernel/AgentFlow/workflows/Code%20Quality/badge.svg?branch=main)](https://github.com/DanielKernel/AgentFlow/actions/workflows/code-quality.yml)
[![codecov](https://codecov.io/gh/DanielKernel/AgentFlow/branch/main/graph/badge.svg)](https://codecov.io/gh/DanielKernel/AgentFlow)
```

---

## 分支保护规则

建议对 `main` 和 `develop` 分支设置以下保护规则：

1. **必须通过状态检查**
   - Build 工作流必须成功
   - Tests 工作流必须成功
   - Code Quality 工作流必须成功

2. **必须有审查批准**
   - 至少需要1个审查批准
   - 代码所有者必须审查 (CODEOWNERS)

3. **强制线性历史**
   - 要求线性提交历史
   - 禁止合并提交

4. **禁止强制推送**
   - 保护历史记录完整性

---

## 查看构建状态

访问 Actions 页面：https://github.com/DanielKernel/AgentFlow/actions

### 平台支持状态

| 平台 | 架构 | 状态 |
|------|------|------|
| Ubuntu | x86_64 (AMD64) | ✅ |
| Ubuntu | ARM64 | ✅ |
| macOS 12 | x86_64 (AMD64) | ✅ |
| macOS 14 | ARM64 (Apple Silicon) | ✅ |

---

## 自动化检查摘要

每次推送和 PR 触发：

1. **构建检查** - 所有平台编译成功
2. **测试执行** - 单元测试、集成测试、内存泄漏检测
3. **代码质量** - 格式检查、静态分析、风格验证
4. **覆盖率分析** - 代码覆盖率报告到 Codecov

---

## 实施完成清单

✅ **工作流文件**: 6个 GitHub Actions workflows
✅ **配置文件**: CODEOWNERS, dependabot.yml, PR/Issue 模板
✅ **平台支持**: Linux AMD64/ARM64, macOS AMD64/ARM64
✅ **代码质量**: clang-format, clang-tidy, cppcheck, cpplint
✅ **测试覆盖**: 单元测试, 集成测试, 内存检测, 覆盖率
✅ **自动发布**: Tag-based release automation
✅ **手动触发**: 可配置的手动构建选项
✅ **文档**: 完整的 CI/CD 指南

---

## 故障排查

### 构建失败
- 检查依赖是否正确安装
- 验证 CMake 配置选项
- 查看详细构建日志

### 测试失败
- 查看失败测试的详细输出
- 检查平台特定问题
- 本地复现测试失败

### 代码质量问题
- 运行 `clang-format` 修复格式
- 使用 `clang-tidy` 修复静态分析问题
- 遵循 Google C++ 风格指南

