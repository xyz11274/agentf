# AgentFlow 测试套件

AgentFlow测试套件提供完整的单元测试、端到端测试、热更新测试和性能测试框架。

**最后更新**: 2026年3月3日

---

## 📁 目录结构

```
tests/
├── README.md                   # 本文档
├── CMakeLists.txt              # 测试构建配置
├── unit/                       # 单元测试
│   ├── src/                    # C++测试源文件
│   │   ├── test_agent_gtest.cpp           # Agent测试
│   │   ├── test_workflow_gtest.cpp        # Workflow测试
│   │   ├── test_node_gtest.cpp            # Node测试
│   │   ├── test_databus_gtest.cpp         # DataBus测试
│   │   ├── test_event_gtest.cpp           # Event测试
│   │   └── test_hot_reload.cpp            # 热更新测试
│   └── data/                   # 测试数据
│       ├── lib/                # SO库测试源码
│       ├── jars/               # JAR测试
│       ├── python/             # Python测试脚本
│       └── shell/              # Shell测试脚本
├── functional/                 # 功能测试
│   └── src/
│       └── test_agent_loading.cpp        # Agent加载测试
├── e2e/                        # 端到端测试
│   ├── README.md               # E2E测试说明
│   ├── CMakeLists.txt          # E2E构建配置
│   ├── message_routing/        # 消息路由场景
│   ├── workflow_execution/     # Workflow执行场景
│   ├── session_isolation/      # Session隔离场景
│   └── data_sharing/           # 数据共享场景
└── perf/                       # 性能测试
    └── test_performance.cpp
```

---

## ✅ 测试套件

### 1. 单元测试

#### MMAgent 测试
**文件**: `unit/legacy_src/test_agent_gtest.cpp`

测试内容:
- MMAgent 创建和销毁
- 生命周期管理
- 状态转换
- 配置管理

运行:
```bash
./build/bin/test_agent_gtest
```

#### MMWorkflow 测试
**文件**: `unit/legacy_src/test_workflow_gtest.cpp`

测试内容:
- MMWorkflow 创建和加载
- 工作流图管理和拓扑排序
- 执行和状态管理
- 条件表达式解析

运行:
```bash
./build/bin/test_workflow_gtest
```

#### MMNode 测试
**文件**: `unit/legacy_src/test_node_gtest.cpp`

测试内容:
- MMNode 创建和配置
- 生命周期管理
- 执行和超时处理
- C++ 函数回调

运行:
```bash
./build/bin/test_node_gtest
```

#### MMDataBus 测试
**文件**: `unit/legacy_src/test_databus_gtest.cpp`

测试内容:
- 零拷贝数据总线
- 订阅/发布机制
- 数据存储和检索
- 并发访问

运行:
```bash
./build/bin/test_databus_gtest
```

#### Event 测试
**文件**: `unit/legacy_src/test_event_gtest.cpp`

测试内容:
- 事件创建和分发
- 事件监听器管理
- 事件队列
- 异步事件处理

运行:
```bash
./build/bin/test_event_gtest
```

---

## 🔗 端到端测试 (E2E Tests)

端到端测试验证完整的业务链路，从 MMAgent 包加载到 MMWorkflow 执行的全流程。

### 测试场景

#### 1. 消息路由场景 (message_routing)

**测试链路**:
```
reqmsg → MMAgent.default_session → MMAgent.new_session → SessionTaskCoordinator → MMAgentTask
```

| 用例ID | 名称 | 描述 |
|--------|------|------|
| TC-E2E-001 | BasicRouting | 基础消息路由（user_id + client_id 创建新 session） |
| TC-E2E-002 | MissingMetadata | 消息 metadata 缺失 user_id/client_id 处理 |
| TC-E2E-003 | CoordinatorWorkflow | coordinator_workflow 默认路由 |
| TC-E2E-004 | ExplicitWorkflowId | 显式 workflow_id 覆盖 |
| TC-E2E-005 | ConcurrentMultiUser | 多用户并发路由 |

#### 2. MMWorkflow 执行场景 (workflow_execution)

**测试链路**:
```
MMAgentTask → Scheduler → Deployer → MMWorkerEngine → Node执行 → 状态更新
```

使用 `examples/agent/basic_agent` 作为测试数据。

| 用例ID | 名称 | 描述 |
|--------|------|------|
| TC-E2E-101 | PackageLoading | 包加载验证 |
| TC-E2E-102 | AgentCreationAndInit | MMAgent 创建和初始化 |
| TC-E2E-103 | AgentLifecycle | MMAgent 生命周期状态转换 |
| TC-E2E-104 | RuntimeRegistrationAndWorkflowExecution | Runtime 注册和 MMWorkflow 执行 |
| TC-E2E-105 | MessageRoutingToWorkflowExecution | 消息路由到 MMWorkflow 执行完整链路 |
| TC-E2E-106 | WorkflowDAGExecution | 多节点 DAG 执行 |
| TC-E2E-107 | CoordinatorWorkflowDefaultRouting | Coordinator MMWorkflow 默认路由 |

#### 3. Session 隔离场景 (session_isolation)

验证不同 Session 之间的隔离性。

| 用例ID | 名称 | 描述 |
|--------|------|------|
| TC-E2E-201 | MultiSessionCreation | 多 Session 并发创建 |
| TC-E2E-202 | TaskIsolation | Session 任务隔离 |
| TC-E2E-203 | MessageQueueIsolation | Session 消息队列隔离 |
| TC-E2E-204 | CoordinatorIsolation | Session coordinator 隔离 |

#### 4. 数据共享场景 (data_sharing)

验证 MMDataBus 跨实例数据共享。

| 用例ID | 名称 | 描述 |
|--------|------|------|
| TC-E2E-301 | InProcessSharing | 进程内数据共享 |
| TC-E2E-302 | DataRefPassing | DataRef 引用传递 |
| TC-E2E-303 | SubscriptionNotify | 订阅通知机制 |
| TC-E2E-304 | OwnerIndexQuery | Owner 索引查询 |

### 运行 E2E 测试

```bash
# 运行所有 E2E 测试
ctest --test-dir build -R E2E --output-on-failure

# 运行特定场景
./build/bin/test_e2e_message_routing
./build/bin/test_e2e_basic_agent

# 运行单个测试用例
./build/bin/test_e2e_basic_agent --gtest_filter=BasicAgentE2ETest.PackageLoading
```

### E2E 测试设计原则

1. **独立性**: 每个测试用例独立运行，不依赖其他用例状态
2. **可重复性**: 测试可多次运行，结果一致
3. **自包含**: 测试数据、配置、预期结果都在用例目录内
4. **真实模拟**: 使用真实的 MMAgent 包（如 basic_agent）而非简单 Mock
5. **链路验证**: 验证完整链路而非单个组件

详细设计文档: [e2e/README.md](e2e/README.md)

---

## 🔥 热更新测试

**文件**: `unit/src/test_hot_reload.cpp`

**支持的可执行文件类型**:
- ✅ **C++ 动态库** (.so/.dylib) - 热更新共享对象
- ✅ **Java JAR** (.jar) - Java Archive
- ✅ **Python 脚本** (.py) - Python脚本
- ✅ **Shell 脚本** (.sh/.bash) - Shell脚本

**测试内容**:
- 文件监控和变更检测
- 版本管理 (最多保留5个版本)
- 动态加载和重载
- 版本回滚
- 自动重载
- 备份功能

**测试用例**: 50+ 个测试

**运行所有热更新测试**:
```bash
./build/bin/test_hot_reload
```

**运行特定类型测试**:
```bash
# SO库测试
./build/bin/test_hot_reload --gtest_filter=HotReloadTest.SoLibrary_*

# Python测试
./build/bin/test_hot_reload --gtest_filter=HotReloadTest.PythonScript_*

# Shell测试
./build/bin/test_hot_reload --gtest_filter=HotReloadTest.ShellScript_*

# JAR测试
./build/bin/test_hot_reload --gtest_filter=HotReloadTest.JarFile_*
```

---

## 📈 性能测试

**文件**: `perf/test_performance.cpp`  
**脚本**: `../scripts/run_performance_tests.sh`

#### 测试场景

| 场景 | 描述 | 负载 |
|------|------|------|
| **基准测试** | 单个Agent，轻负载 | 100个简单工作流 |
| **并发测试** | 多Agent并发执行 | 4个Agent，1000个工作流 |
| **压力测试** | 高负载持续执行 | 10000个复杂工作流 |
| **稳定性测试** | 长期运行测试 | 连续运行5分钟 |

#### 性能指标

**线程管理**:
- 峰值线程数
- 平均线程数
- 最小线程数

**资源使用**:
- 峰值内存 (MB)
- 平均内存 (MB)
- 峰值 CPU (%)
- 平均 CPU (%)

**调度性能**:
- 平均调度延迟 (ms)
- P50 延迟 (中位数)
- P95 延迟
- P99 延迟

**系统稳定性**:
- 内存增长率
- 线程数稳定性

#### 运行性能测试

**自动化测试**（推荐）:
```bash
cd scripts
./run_performance_tests.sh
```

输出示例:
```
==========================================
         AgentFlow 性能测试
==========================================
测试环境:
  操作系统: Darwin
  CPU核心数: 8
  总内存: 16384 MB
  可用内存: 8192 MB
------------------------------------------
[场景 1/4] 基准测试
  工作流数: 100
  ✓ 通过 (1.2s)
  峰值内存: 45.2 MB | 平均CPU: 12%
------------------------------------------
[场景 2/4] 并发测试
  工作流数: 1000 (4个Agent)
  ✓ 通过 (8.5s)
  峰值内存: 128.5 MB | 平均CPU: 45%
------------------------------------------
[场景 3/4] 压力测试
  工作流数: 10000
  ✓ 通过 (65.3s)
  峰值内存: 512.8 MB | 平均CPU: 78%
------------------------------------------
[场景 4/4] 稳定性测试
  持续时间: 300s
  ✓ 通过 (300.1s)
  内存增长: +2.3 MB | 线程稳定: ✓
==========================================
总结: 4/4 通过 ✓
==========================================
```

**手动测试**:
```bash
cd build
./bin/test_performance
```

#### 性能基线

当前性能基线（M1 MacBook Pro, 8核）:

| 测试场景 | 吞吐量 | 平均延迟 | P99延迟 | 内存 |
|---------|--------|---------|---------|------|
| 基准测试 | 83 workflow/s | 12ms | 18ms | 45MB |
| 并发测试 | 118 workflow/s | 8.5ms | 15ms | 128MB |
| 压力测试 | 153 workflow/s | 6.5ms | 12ms | 512MB |

**性能优化建议**:
- 调度延迟 < 10ms (目标: 5ms)
- P99 延迟 < 50ms (目标: 20ms)
- 内存增长率 < 1MB/min
- CPU 使用率 < 80% (平均)

---

## 🚀 快速开始

### 构建所有测试

```bash
# 从项目根目录
./build.sh

# 或者手动构建
cd build
cmake ..
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
```

### 运行所有测试

```bash
cd build
ctest --output-on-failure
```

### 运行特定测试

```bash
# 单个测试套件
./build/bin/test_agent_gtest
./build/bin/test_workflow_gtest
./build/bin/test_node_gtest

# 使用过滤器
./build/bin/test_node_gtest --gtest_filter=NodeTests.Lifecycle
```

---

## 📊 测试状态

**最新测试结果** (2026-02-18):

| 测试套件 | 状态 | 测试数 | 说明 |
|---------|------|--------|------|
| NodeTests | ✅ 通过 | 15 | 所有测试通过 |
| AgentTests | ✅ 通过 | 12 | 所有测试通过 |
| WorkflowTests | ✅ 通过 | 18 | 所有测试通过 |
| DataBusTests | ✅ 通过 | 10 | 所有测试通过 |
| EventTests | ✅ 通过 | 8 | 所有测试通过 |
| AdapterIsolationTests | ✅ 通过 | 5 | 所有测试通过 |
| AgentFlowGoogleTests | ⚠️ 失败 | - | SEGFAULT (预存在) |
| HotReloadTests | ⚠️ 失败 | - | 时序问题 (预存在) |
| EventLoopTests | ⚠️ 失败 | - | SEGFAULT (预存在) |
| PerformanceTests | ✅ 通过 | 4 | 所有场景通过 |

**总计**: 7/10 测试套件通过 (70%)

---

## 🔧 故障排查

### 常见问题

#### 1. 测试找不到共享库

**问题**: 
```
dyld: Library not loaded: @rpath/libagentflow.dylib
```

**解决方案**:
```bash
# macOS
export DYLD_LIBRARY_PATH=$PWD/build/lib:$DYLD_LIBRARY_PATH

# Linux
export LD_LIBRARY_PATH=$PWD/build/lib:$LD_LIBRARY_PATH
```

#### 2. 热更新测试失败

**问题**: SO库或脚本文件未找到

**解决方案**:
```bash
# 确保测试资源已构建
cd tests/lib && make
cd tests/jars && ./build.sh
```

#### 3. 性能测试超时

**问题**: 压力测试在低配机器上超时

**解决方案**:
```bash
# 调整工作流数量
./build/bin/test_performance --gtest_filter=*Baseline*  # 仅运行基准测试
```

---

## 📚 相关文档

| 文档 | 内容 |
|------|------|
| [../docs/README.md](../docs/README.md) | 测试策略和最佳实践 |
| [../CONTRIBUTING.md](../CONTRIBUTING.md) | 开发指南 |
| [../docs/api-reference.md](../docs/api-reference.md) | API 参考 |
| [../docs/README.md](../docs/README.md) | CI/CD 集成 |

---

## 🤝 贡献测试

### 添加新测试

1. **单元测试**: 在 `tests/unit/legacy_src/` 创建 `test_<component>_gtest.cpp`
2. **性能测试**: 在 `test_performance.cpp` 添加新场景
3. **热更新测试**: 在 `test_hot_reload.cpp` 添加新类型支持

### 测试命名规范

```cpp
// GoogleTest命名: TestSuiteName.TestName
TEST(AgentTests, CreateAgent) { ... }
TEST(WorkflowTests, ExecuteDAG) { ... }
TEST(NodeTests, Lifecycle) { ... }
```

### 提交前检查

```bash
# 1. 运行所有测试
cd build && ctest --output-on-failure

# 2. 运行性能测试
cd scripts && ./run_performance_tests.sh

# 3. 检查代码格式
clang-format -i tests/**/*.cpp
```

---

**维护者**: @DanielKernel  
**测试框架**: Google Test 1.11.0  
**CI集成**: GitHub Actions
