# AgentFlow 依赖隔离策略

## 概述

AgentFlow项目依赖于Protoactor-cpp框架，但为了确保项目稳定性和可维护性，需要对外部依赖进行隔离。这份文档描述了隔离策略和实现方式。

## 核心原则

1. **适配器模式**：所有对Protoactor-cpp的调用都通过适配器（Adapter）层进行
2. **接口隔离**：AgentFlow内部只依赖抽象接口，不依赖具体实现
3. **版本兼容**：适配器层负责处理Protoactor-cpp版本差异
4. **可测试性**：提供Mock实现用于单元测试，避免外部依赖
5. **条件编译**：在没有Protoactor-cpp的情况下，使用存根实现保证编译

## 架构设计

### 隔离层结构

```
┌─────────────────────────────────┐
│  AgentFlow 核心 (common.h...)   │
│  - Agent, Workflow, Node        │
│  - DataBus, EventManager        │
└────────────┬────────────────────┘
             │ (仅依赖)
┌────────────▼────────────────────┐
│ 适配器接口层 (adapter.h)        │
│  - IActor                       │
│  - IContext                     │
│  - IActorSystem                 │
└────────────┬────────────────────┘
             │ (隐藏实现)
┌────────────▼────────────────────┐
│ Protoactor适配器实现             │
│  (WITH_PROTOACTOR=1时编译)      │
│  - ProtoactorActorImpl           │
│  - ProtoactorContextImpl         │
│  - ProtoactorSystemImpl          │
└─────────────────────────────────┘
             │ (直接依赖)
┌────────────▼────────────────────┐
│ Protoactor-cpp框架             │
│ (外部项目,可选)                 │
└─────────────────────────────────┘
```

### 接口定义示例

```cpp
namespace agentflow::actor::adapter {

/// 抽象Actor接口 - AgentFlow内部使用
class IActor {
public:
  virtual ~IActor() = default;
  virtual void on_receive(IContext* ctx) = 0;
  virtual std::string get_actor_type() const = 0;
};

/// 抽象Context接口
class IContext {
public:
  virtual ~IContext() = default;
  virtual std::shared_ptr<IActorRef> self() = 0;
  virtual std::shared_ptr<IActorRef> sender() = 0;
  virtual void send(IActorRef* target, Message* msg) = 0;
};

/// 抽象ActorSystem接口
class IActorSystem {
public:
  virtual ~IActorSystem() = default;
  virtual std::shared_ptr<IActorRef> spawn(ActorFactory factory) = 0;
  virtual void shutdown() = 0;
};

}
```

## 实现策略

### 1. 条件编译

```cpp
#ifdef AGENTFLOW_WITH_PROTOACTOR
  #include "adapter/protoactor_impl.h"
#else
  #include "adapter/stub_impl.h"
#endif
```

### 2. 版本适配

在适配器中定义版本检查：

```cpp
static_assert(PROTOACTOR_VERSION >= 1000,  // v1.0.0
              "Protoactor-cpp version 1.0.0 or later required");
```

如果上游版本变化，只需在适配器中修改，不影响AgentFlow核心。

### 3. 工厂模式

```cpp
class ActorSystemFactory {
public:
  static std::shared_ptr<IActorSystem> create();
  // 版本2.0.0发布时，可轻松更换实现
};
```

### 4. 通用消息格式

定义与Protoactor版本无关的消息类型：

```cpp
struct AgentFlowMessage {
  std::string type;
  std::shared_ptr<void> payload;
  // 不依赖Protoactor的任何消息格式
};
```

## 变更隔离成本

| 变更类型 | 影响范围 | 隔离成本 |
|---------|---------|---------|
| Protoactor API改变 | 仅适配器层 | 低 |
| Protoactor行为变化 | 仅适配器层 | 中 |
| Protoactor性能改进 | 自动受益，无需修改 | 无 |
| Protoactor重大版本更新 | 仅适配器层+新实现类 | 中 |
| Protoactor删除功能 | 适配器层+CoreImpl修改 | 高（罕见） |

## 文件说明

### 核心隔离文件

- `include/agentflow/actor/adapter.h` - 适配器接口定义
- `include/agentflow/actor/adapter/protoactor_impl.h` - Protoactor实现
- `include/agentflow/actor/adapter/stub_impl.h` - 独立实现（无Protoactor时）
- `src/actor/adapter/*.cpp` - 适配器实现

### 隔离测试

- `tests/test_adapter_isolation.cpp` - 验证代码正常隔离
- `tests/mock/mock_actor_system.h` - Mock实现用于单元测试

## 使用示例

AgentFlow内部代码只依赖适配器接口：

```cpp
// ✓ 允许 - 仅使用适配器接口
#include "agentflow/actor/adapter.h"

void AgentImpl::execute() {
  auto system = adapter::ActorSystemFactory::create();
  auto actor_ref = system->spawn(my_actor_factory);
  system->send(actor_ref, my_message);
}

// ✗ 禁止 - 直接依赖Protoactor
#include "protoactor/actor_system.h"  // 不允许直接包含
```

## 迁移到新的Actor框架

如果未来需要从Protoactor迁移到其他Actor框架，只需：

1. 创建新的适配器实现：`adapter/new_framework_impl.h`
2. 在CMakeLists.txt中选择不同的实现
3. 核心AgentFlow代码无需修改

示例：迁移到Akka.NET、Erlang/OTP或其他框架

```cpp
// 旧实现
#if defined(AGENTFLOW_WITH_PROTOACTOR)
  #include "adapter/protoactor_impl.h"
#elif defined(AGENTFLOW_WITH_AKKA)
  #include "adapter/akka_impl.h"
#else
  #include "adapter/stub_impl.h"
#endif
```

## 维护指南

### 添加新的Actor功能

1. **第一步**：在 `adapter.h` 中定义接口
2. **第二步**：在 `adapter/protoactor_impl.h` 中实现
3. **第三步**：在 `adapter/stub_impl.h` 中提供存根
4. **第四步**：在AgentFlow内部使用该接口

### 处理Protoactor升级

1. 检查API变化
2. 更新 `adapter/protoactor_impl.h`
3. 运行隔离测试验证
4. 更新版本要求

### 添加新Actor框架支持

1. 创建 `adapter/new_framework_impl.h`
2. 实现所有adapter接口
3. 添加CMake选项
4. 创建对应的单元测试

## 测试隔离性

```bash
# 测试仅使用adapter接口的代码编译
g++ -DAGENTFLOW_WITH_PROTOACTOR=0 ...

# 使用Mock实现验证功能
# 见 tests/mock/mock_actor_system.h
```

## 总结

通过严格的适配器隔离策略，AgentFlow具备以下优势：

✓ 独立于Protoactor-cpp版本
✓ 轻松应对上游API变化
✓ 支持多个Actor框架共存
✓ 便于单元测试（使用Mock）
✓ 清晰的职责边界
✓ 代码重用性高

---

**维护者**: AgentFlow Team
**更新时间**: 2026-02-16
**相关文档**: [PROTOACTOR_INSTALL.md](PROTOACTOR_INSTALL.md)
