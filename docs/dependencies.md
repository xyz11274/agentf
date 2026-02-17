# AgentFlow 依赖管理

> 本文档整合了 AgentFlow 项目的外部依赖说明、依赖隔离策略和 Protoactor-cpp 安装指南。

---

## 目录

1. [依赖概览](#1-依赖概览)
2. [可选依赖详解](#2-可选依赖详解)
3. [依赖安装指南](#3-依赖安装指南)
4. [依赖隔离策略](#4-依赖隔离策略)
5. [Stub 实现说明](#5-stub-实现说明)
6. [故障排除](#6-故障排除)

---

## 1. 依赖概览

### 1.1 依赖分类

| 类型 | 依赖 | 必需/可选 | 说明 |
|------|------|-----------|------|
| **核心** | C++17 标准库 | 必需 | 基础数据结构和算法 |
| **核心** | CMake >= 3.10 | 必需 | 构建系统 |
| **核心** | Threads | 必需 | 多线程支持 |
| **可选** | Protoactor-cpp | 可选 | Actor 模型实现 |
| **可选** | OpenSSL | 可选 | SSL/TLS 加密 |
| **可选** | libwebsockets | 可选 | WebSocket 支持 |
| **可选** | libevent | 可选 | 事件循环 |
| **可选** | RapidJSON | 可选 | JSON 解析 |
| **可选** | Protobuf | 可选 | 序列化 |
| **测试** | Google Test | 可选 | 单元测试框架 |

### 1.2 依赖检测机制

项目通过 CMake 自动检测这些依赖，如果未找到则使用 stub 实现：

```cmake
# 核心依赖
find_package(Threads REQUIRED)

# 可选依赖（未找到时使用stub）
find_package(protoactor CONFIG QUIET)
find_package(OpenSSL QUIET)
find_package(libwebsockets CONFIG QUIET)
find_package(Protobuf QUIET)
find_package(libevent CONFIG QUIET)

# RapidJSON (header-only)
find_path(RAPIDJSON_INCLUDE_DIR NAMES rapidjson/document.h)
```

---

## 2. 可选依赖详解

### 2.1 Protoactor-cpp（Actor 框架）

- **GitHub**: https://github.com/DanielKernel/Protoactor-cpp.git
- **用途**: Actor 模型实现，提供分布式 Actor 通信和消息机制
- **C++ 标准**: C++17

**核心依赖**：

| 依赖 | 版本要求 | 用途 |
|------|---------|------|
| grpc | >= 1.30 | gRPC 通信库，用于远程 Actor 通信 |
| protobuf | >= 3.10 | Protocol Buffer，用于消息序列化 |
| openssl | >= 1.1 | SSL/TLS 加密，安全通信支持 |
| zlib | >= 1.2 | 数据压缩，减少网络传输数据量 |

**编译要求**：
- CMake >= 3.10
- GCC 7+ / Clang 5+ / MSVC 2017+

**支持平台**：
- Linux (x86_64, ARM64)
- macOS (x86_64, Apple Silicon ARM64)
- Windows (x86_64, CMake 支持)

### 2.2 OpenSSL（SSL/TLS 加密）

- **官网**: https://www.openssl.org/
- **用途**: SSL/TLS 安全通信
- **功能**: SSL/TLS 协议支持、证书管理、加密通信

### 2.3 libwebsockets（WebSocket 支持）

- **官网**: https://libwebsockets.org/
- **用途**: WebSocket 协议实现
- **功能**: WebSocket 服务器和客户端、实时双向通信

### 2.4 libevent（事件循环）

- **官网**: https://libevent.org/
- **用途**: 高性能事件循环
- **功能**: 跨平台事件循环、高性能 I/O 多路复用

### 2.5 RapidJSON（JSON 解析）

- **GitHub**: https://github.com/Tencent/rapidjson
- **用途**: JSON 序列化和反序列化
- **特点**: Header-only 库，高性能 JSON 解析

### 2.6 Protobuf（序列化）

- **官网**: https://developers.google.com/protocol-buffers
- **用途**: 二进制序列化协议
- **特点**: 高效的二进制序列化，跨语言支持

---

## 3. 依赖安装指南

### 3.1 Ubuntu/Debian

```bash
# 核心编译工具
sudo apt-get install -y build-essential cmake git pkg-config

# Protoactor-cpp 依赖
sudo apt-get install -y \
    libgrpc++-dev \
    libgrpc-dev \
    libprotobuf-dev \
    protobuf-compiler \
    libssl-dev \
    zlib1g-dev

# AgentFlow 可选依赖
sudo apt-get install -y \
    libwebsockets-dev \
    libevent-dev \
    rapidjson-dev

# 测试框架
sudo apt-get install -y libgtest-dev

# 编译安装 Protoactor-cpp
git clone https://github.com/DanielKernel/Protoactor-cpp.git
cd Protoactor-cpp && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
```

### 3.2 macOS（Homebrew）

```bash
# 核心编译工具
xcode-select --install
brew install cmake git

# Protoactor-cpp 依赖
brew install grpc protobuf openssl zlib

# AgentFlow 可选依赖
brew install libwebsockets libevent rapidjson

# 测试框架
brew install googletest

# 设置 OpenSSL 路径（macOS 可能需要）
export OPENSSL_ROOT_DIR=$(brew --prefix openssl)

# 编译安装 Protoactor-cpp
git clone https://github.com/DanielKernel/Protoactor-cpp.git
cd Protoactor-cpp && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu)
sudo make install
```

### 3.3 CentOS/RHEL/Fedora

```bash
# Fedora
sudo dnf install -y \
    gcc-c++ cmake git \
    grpc-devel protobuf-devel \
    openssl-devel zlib-devel \
    libevent-devel rapidjson-devel \
    libwebsockets-devel \
    gtest-devel

# CentOS/RHEL（需要 EPEL）
sudo yum install -y epel-release
sudo yum install -y \
    gcc-c++ cmake git \
    grpc-devel protobuf-devel \
    openssl-devel zlib-devel \
    libevent-devel
```

### 3.4 Arch Linux

```bash
sudo pacman -S --noconfirm \
    base-devel cmake git \
    grpc protobuf openssl zlib \
    libevent rapidjson libwebsockets \
    gtest
```

---

## 4. 依赖隔离策略

### 4.1 核心原则

1. **适配器模式**：所有对外部依赖的调用都通过适配器层进行
2. **接口隔离**：AgentFlow 内部只依赖抽象接口，不依赖具体实现
3. **版本兼容**：适配器层负责处理上游版本差异
4. **可测试性**：提供 Mock 实现用于单元测试，避免外部依赖
5. **条件编译**：在没有外部依赖的情况下，使用存根实现保证编译

### 4.2 隔离层架构

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
│ 实现选择 (条件编译)              │
│  WITH_PROTOACTOR ? 完整实现     │
│  : stub 实现                    │
└─────────────────────────────────┘
```

### 4.3 接口定义示例

```cpp
namespace agentflow::actor::adapter {

/// 抽象 Actor 接口 - AgentFlow 内部使用
class IActor {
public:
  virtual ~IActor() = default;
  virtual void on_receive(IContext* ctx) = 0;
  virtual std::string get_actor_type() const = 0;
};

/// 抽象 ActorSystem 接口
class IActorSystem {
public:
  virtual ~IActorSystem() = default;
  virtual std::shared_ptr<IActorRef> spawn(ActorFactory factory) = 0;
  virtual void shutdown() = 0;
};

}
```

### 4.4 条件编译

```cpp
#ifdef AGENTFLOW_WITH_PROTOACTOR
  #include "adapter/protoactor_impl.h"
#else
  #include "adapter/stub_impl.h"
#endif
```

### 4.5 工厂模式

```cpp
class ActorSystemFactory {
public:
  static std::shared_ptr<IActorSystem> create() {
#ifdef AGENTFLOW_WITH_PROTOACTOR
    return std::make_shared<ProtoactorSystemImpl>();
#else
    return std::make_shared<StubActorSystemImpl>();
#endif
  }
};
```

---

## 5. Stub 实现说明

### 5.1 概述

对于每个可选依赖，AgentFlow 都提供了 stub 实现：

- **作用**: 在依赖库未安装时提供基本功能
- **限制**: 功能受限，仅用于测试和基本场景
- **使用**: CMake 自动检测依赖，未找到时使用 stub

### 5.2 Stub 实现列表

| 依赖 | Stub 文件 | 说明 |
|------|-----------|------|
| Actor | `src/actor/stub_impl.cpp` | 基本Actor模拟 |
| SSL | `include/.../openssl_impl.h` | 无加密通信 |
| WebSocket | `include/.../libwebsockets_impl.h` | 无 WebSocket 支持 |
| Event Loop | `include/.../libevent_impl.h` | 简单事件处理 |
| JSON | `src/dsl/json_adapter_stub.cpp` | 基础 JSON 解析 |
| Protobuf | `include/.../protobuf_impl.h` | 无序列化 |

### 5.3 适配器模式示例

```cpp
// 接口定义（项目内部）
class ISslAdapter { ... };

// 具体实现（第三方库）
class SslOpensslAdapter : public ISslAdapter { ... };

// Stub 实现（降级方案）
class SslStubAdapter : public ISslAdapter { ... };

// 工厂方法（根据依赖选择实现）
std::shared_ptr<ISslAdapter> create_ssl_adapter() {
#ifdef AGENTFLOW_WITH_OPENSSL
  return std::make_shared<SslOpensslAdapter>();
#else
  return std::make_shared<SslStubAdapter>();
#endif
}
```

---

## 6. 故障排除

### 6.1 找不到 gRPC 库

```bash
# Ubuntu/Debian
sudo apt-get install libgrpc++-dev libgrpc-dev

# macOS
brew install grpc
```

### 6.2 Protobuf 版本冲突

```bash
# 检查版本
protoc --version

# 重新安装
sudo apt-get remove libprotobuf-dev protobuf-compiler
sudo apt-get install libprotobuf-dev protobuf-compiler

# macOS
brew upgrade protobuf
```

### 6.3 OpenSSL 找不到（macOS）

```bash
brew install openssl
export OPENSSL_ROOT_DIR=$(brew --prefix openssl)

# 或在 cmake 时指定
cmake .. -DOPENSSL_ROOT_DIR=$(brew --prefix openssl)
```

### 6.4 运行时找不到共享库

```bash
# Linux
sudo ldconfig /usr/local/lib
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH

# macOS
export DYLD_LIBRARY_PATH=/usr/local/lib:$DYLD_LIBRARY_PATH
```

### 6.5 CMake 找不到 Protoactor

```bash
# 验证安装
ls -l /usr/local/lib/libprotoactor*
ls -l /usr/local/include/protoactor/

# 设置 CMAKE_PREFIX_PATH
cmake .. -DCMAKE_PREFIX_PATH=/usr/local
```

### 6.6 自定义安装路径

```bash
# 安装到自定义路径
cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/.local
make install

# 配置环境
export PATH=$HOME/.local/bin:$PATH
export LD_LIBRARY_PATH=$HOME/.local/lib:$LD_LIBRARY_PATH
export PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig:$PKG_CONFIG_PATH
```

---

## 7. 注意事项

1. **不要重新实现第三方库**: 这些库都有成熟实现，AgentFlow 仅提供适配和集成
2. **依赖冲突**: 确保所有依赖的版本兼容
3. **编译顺序**: Protoactor-cpp 需要先编译，其他库可通过包管理器安装
4. **生产环境**: 建议启用所有依赖，使用完整功能而非 stub
5. **许可证**: 第三方库有自己的开源许可证，使用时请遵守相应协议

---

## 8. 文件说明

### 核心隔离文件

| 文件 | 说明 |
|------|------|
| `include/agentflow/actor/adapter.h` | 适配器接口定义 |
| `include/agentflow/actor/adapter/protoactor_impl.h` | Protoactor 实现 |
| `include/agentflow/actor/adapter/stub_impl.h` | Stub 实现 |
| `src/actor/adapter/*.cpp` | 适配器实现 |

### 隔离测试

| 文件 | 说明 |
|------|------|
| `tests/test_adapter_isolation.cpp` | 验证代码正常隔离 |
| `tests/mock/mock_actor_system.h` | Mock 实现用于单元测试 |

---

*最后更新: 2026-02-17*
