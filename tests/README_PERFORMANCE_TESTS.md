# AgentFlow 性能测试指南

本文档详细说明AgentFlow的自动化性能测试框架、运行方式和指标解读。

## 目录

1. [概述](#概述)
2. [性能测试框架](#性能测试框架)
3. [测试场景](#测试场景)
4. [性能指标](#性能指标)
5. [运行测试](#运行测试)
6. [CI/CD集成](#cicd集成)
7. [性能基线](#性能基线)
8. [调试和优化](#调试和优化)

---

## 概述

AgentFlow包含全面的自动化性能测试框架，用于：

- **监控性能回归** - 检测新代码引入的性能问题
- **基准测试** - 建立进行中的性能基线
- **容量规划** - 预测系统在高负载下的行为
- **优化验证** - 验证优化工作的效果

性能测试涵盖以下关键指标：

| 指标类别 | 监控指标 |
|---------|---------|
| **线程管理** | 峰值线程数、平均线程数、最小线程数 |
| **资源使用** | 峰值内存、平均内存、峰值CPU、平均CPU |
| **调度性能** | 平均延迟、P50、P95、P99延迟 |
| **系统稳定性** | 长期运行的内存增长、线程计数稳定性 |

---

## 性能测试框架

### 核心组件

#### 1. `PerformanceMetrics` 类
包含完整的性能指标定义和输出格式。

```cpp
struct PerformanceMetrics {
    // 线程指标
    struct ThreadMetrics {
        int peak_threads;      // 峰值线程数
        int avg_threads;       // 平均线程数
        int min_threads;       // 最小线程数
    };

    // 资源指标
    struct ResourceMetrics {
        double peak_memory_mb;    // 峰值内存(MB)
        double avg_memory_mb;     // 平均内存(MB)
        double peak_cpu_percent;  // 峰值CPU(%)
        double avg_cpu_percent;   // 平均CPU(%)
    };

    // 延迟指标
    struct LatencyMetrics {
        double total_duration_ms;      // 总执行时间
        double schedule_latency_ms;    // 平均调度延迟
        double execution_latency_ms;   // 平均执行时间
        double p50_latency_ms;         // P50(中位数)
        double p95_latency_ms;         // P95上边界
        double p99_latency_ms;         // P99上边界
    };
};
```

#### 2. `SystemMonitor` 类
从系统获取实时性能数据。

**主要函数：**

| 函数 | 功能 |
|------|------|
| `get_thread_count()` | 获取当前线程数 |
| `get_memory_usage_mb()` | 获取当前内存使用(MB) |
| `get_cpu_usage_percent()` | 获取当前CPU使用率(%) |
| `collect_samples()` | 定期采集系统资源样本 |

**采样数据结构：**
```cpp
struct Sample {
    int threads;                              // 线程计数
    double memory_mb;                         // 内存使用
    double cpu_percent;                       // CPU使用率
    std::chrono::steady_clock::time_point timestamp;  // 时间戳
};
```

#### 3. `PerformanceTester` 类
组织和计算性能指标。

**主要方法：**

```cpp
// 记录事件时间戳（用于延迟分析）
void record_event_timestamp(const std::string& event_name);

// 计算百分位数
double calculate_percentile(const std::vector<double>& values, double percentile);

// 从采样数据计算指标
PerformanceMetrics compute_metrics(
    int num_agents,
    int num_workflows,
    int num_nodes,
    int num_iterations,
    const std::vector<SystemMonitor::Sample>& samples,
    const std::vector<double>& schedule_latencies,
    const std::vector<double>& execution_latencies
);
```

---

## 测试场景

### 场景1: 单Agent单Workflow多Node

**配置：**
- Agent数: 1
- Workflow数: 1
- Node数: 10
- 迭代次数: 100

**目的:** 建立基础性能基线

**关键指标目标：**
- 峰值内存: < 200MB
- 调度延迟P99: < 5ms
- CPU使用率: < 50%

```cpp
TEST_F(PerformanceTestFixture, SingleAgentSingleWorkflowMultiNode) {
    // 采集30秒的系统资源样本
    auto samples = SystemMonitor::collect_samples(30000, 10);
    
    // 计算性能指标
    auto metrics = tester.compute_metrics(1, 1, 10, 100, samples, 
                                          schedule_latencies, execution_latencies);
    
    // 验证性能基线
    EXPECT_LT(metrics.resource_metrics.peak_memory_mb, 200.0);
}
```

### 场景2: 单Agent多Workflow

**配置：**
- Agent数: 1
- Workflow数: 5
- Node数: 25 (每个Workflow 5个)
- 迭代次数: 50

**目的:** 评估单Agent内多Workflow的并行能力

**关键指标目标：**
- 峰值内存: < 400MB
- Workflow并行化效率: > 80%
- 调度延迟P99: < 8ms

### 场景3: 多Agent多Workflow (高并发)

**配置：**
- Agent数: 3
- Workflow数: 5
- Node数: 40 (总计)
- 迭代次数: 50

**目的:** 验证Agent级别的协调和资源争抢处理

**关键指标目标：**
- 峰值内存: < 800MB
- 线程数: < 80
- CPU扩展性: 线形扩展(< 1.2倍)

### 场景4: 长期稳定性测试

**配置：**
- Agent数: 2
- Workflow数: 3
- Node数: 5
- 迭代次数: 1000
- 持续时间: 60秒

**目的:** 检测内存泄漏和长期运行的稳定性

**关键指标目标：**
- 内存增长: < 200MB (整个测试过程)
- 线程计数: 稳定(峰值和最小值比例 < 1.5x)
- CPU稳定: 无突发波动

### 场景5: 调度延迟压力测试

**配置：**
- Agent数: 1
- Workflow数: 1
- Node数: 20
- 迭代次数: 500

**目的:** 验证高频率任务调度的延迟特性

**关键指标目标：**
- P50延迟: < 2ms
- P95延迟: < 5ms
- P99延迟: < 10ms

---

## 性能指标

### 线程指标

| 指标 | 含义 | 解读 |
|------|------|------|
| Peak Threads | 执行期间的最高线程数 | 衡量并发度，应合理控制 |
| Avg Threads | 平均线程数 | 显示通常的并发水平 |
| Min Threads | 最低线程数 | 应>= 基础线程数 |

**解读指南：**
- 峰值线程数过高(> 100) → 考虑线程池大小优化
- 线程抖动大(峰值-最小 > 50) → 可能有频繁的线程创建/销毁

### 资源指标

#### 内存使用

- **Peak Memory:** 执行期间的最大内存占用
  - 目标: < 1GB (多Agent场景)
  - 过高说明: 内存泄漏或数据结构过大

- **Avg Memory:** 平均内存占用
  - 用于容量规划
  - 增长率应该逐渐缓和

#### CPU使用

- **Peak CPU:** 最高CPU使用率(%)
  - 目标: < 100% (足够的并发度)
  - 过高说明: CPU成为瓶颈或有繁忙轮询

- **Avg CPU:** 平均CPU使用率
  - 衡量整体利用效率
  - 应该 > 50% (指示良好的并行化)

### 延迟指标

延迟用5个关键数值表示：

1. **Average (平均值)**
   - 所有样本的平均延迟
   - 易受异常值影响

2. **P50 (中位数, 第50百分位)**
   - 50%的操作在此时间内完成
   - 代表"典型"情况

3. **P95 (第95百分位)**
   - 95%的操作在此时间内完成
   - 显示"较好"的情况

4. **P99 (第99百分位)**
   - 99%的操作在此时间内完成
   - 显示"最坏"的目标

5. **Total Duration (总耗时)**
   - 整个测试的执行时间

**延迟目标：**
```
Scheduling Latency:     P99 < 10ms
Execution Latency:      P99 < 20ms
Task Completion:        P99 < 50ms
```

---

## 运行测试

### 本地运行性能测试

#### 1. 单个性能测试

```bash
# 构建
cd /path/to/AgentFlow
./build.sh build

# 运行特定性能测试
cd build
./test_performance --gtest_filter="Performance*SingleAgent*"
```

#### 2. 所有性能测试

```bash
cd build
./test_performance --gtest_filter="Performance*"
```

#### 3. 使用性能脚本运行完整测试套件

```bash
bash scripts/run_performance_tests.sh
```

**脚本输出：**
```
Performance Test Results
  system_info.txt         - 系统配置信息
  performance_test.log    - 原始测试输出
  performance_results.json - 结构化JSON结果
  performance_report.md   - 可读的分析报告
```

### 浏览结果

```bash
# 查看报告
cat build/performance_results/performance_report.md

# 查看详细日志
cat build/performance_results/performance_test.log

# 查看系统信息
cat build/performance_results/system_info.txt
```

---

## CI/CD集成

### GitHub Actions 工作流

性能测试在 `.github/workflows/tests.yml` 中自动运行。

#### 触发条件

- **Push到main/develop分支** (改动影响src/, include/, tests/)
- **Pull Requests** (同上条件)
- **手动触发** (通过GitHub UI)

#### 工作流步骤

1. **系统检查** - 记录CPU核数、内存大小
2. **构建** - Release配置编译
3. **运行测试** - 执行所有性能测试
4. **收集指标** - 汇总结果
5. **生成报告** - 生成可读的分析报告
6. **上传结果** - 保存为制品 (30天保留)
7. **PR注释** (仅PR) - 在PR中评论性能结果

#### 查看CI/CD结果

1. **GitHub Actions 仪表板**
   - 访问: https://github.com/DanielKernel/AgentFlow/actions
   - 选择: "Tests" 工作流
   - 查看: "performance-testing" 任务

2. **PR评论**
   - 自动在PR中评论性能结果
   - 包含关键指标摘要

3. **下载报告**
   - Actions → "performance-testing" → "Artifacts"
   - 下载 "performance-report-{SHA}"

---

## 性能基线

### 已确立的基线

基于开发机(4核CPU, 8GB内存)的基线：

| 场景 | 峰值内存 | P99延迟 | CPU利用 | 线程数 |
|------|---------|--------|--------|--------|
| Single/Single/10Node | < 200MB | < 5ms | < 50% | < 20 |
| Single/5Workflows | < 400MB | < 8ms | < 70% | < 40 |
| Multi(3)/5Workflows | < 800MB | < 12ms | < 90% | < 70 |
| StabilityTest(60s) | +200MB | - | < 30% | ±10 |
| SchedulingLatency | < 100MB | < 10ms | < 20% | < 15 |

### 确定性能回归

性能回归定义为：

1. **延迟增加** > 20%
2. **内存增长** > 20%
3. **线程爆炸** > 50%

注：CI/CD会自动检查这些条件并标记回归。

---

## 调试和优化

### 性能分析工作流

```
性能测试失败
    ↓
查看性能报告确定问题
    ↓
使用profiling工具分析 (perf, valgrind)
    ↓
优化代码
    ↓
验证改进 (本地运行性能测试)
    ↓
提交PR (CI/CD验证)
```

### 常见性能问题

#### 问题1: 线程数过多
**症状：** Peak Threads > 100
**原因：** 
- 线程池大小配置过大
- 频繁创建/销毁线程
**解决：**
```cpp
// 调整线程池大小
int optimal_threads = std::thread::hardware_concurrency();
```

#### 问题2: 内存持续增长
**症状：** Avg Memory 在60s增长 > 200MB
**原因：**
- 内存泄漏
- 缓存无界增长
**解决：**
```bash
# 使用Valgrind检查内存泄漏
valgrind --leak-check=full --show-leak-kinds=all ./test_performance
```

#### 问题3: 调度延迟高
**症状：** P99 Latency > 20ms
**原因：**
- 任务队列争抢
- 调度器开销大
**解决：**
```cpp
// 使用分片队列减少争抢
// 考虑锁自由数据结构
```

### 常用Profiling工具

#### perf (Linux)
```bash
# 记录性能事件
perf record -F 99 ./test_performance

# 分析结果
perf report
```

#### valgrind
```bash
# 内存分析
valgrind --tool=massif --massif-out-file=massif.out ./test_performance

# 查看结果
ms_print massif.out
```

#### Google pprof
```bash
# 需要在代码中集成pprof
./test_performance --binary=/path/to/binary
google-pprof --pdf /path/to/binary profile.out
```

---

## 参考资源

- 完整的CI/CD文档: [CI_CD_PIPELINE.md](../docs/CI_CD_PIPELINE.md)
- 架构设计: [ARCHITECTURE.md](../docs/ARCHITECTURE.md)
- 快速参考: [QUICK_REFERENCE.md](../docs/QUICK_REFERENCE.md)

---

**最后更新：** 2026-02-17
**维护者：** @DanielKernel
