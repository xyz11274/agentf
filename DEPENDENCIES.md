# AgentFlow 外部依赖说明

本文档列出了AgentFlow项目的外部依赖库，这些库由第三方实现，不需要在AgentFlow项目中重新实现。

---

## 一、核心依赖（必需）

### 1. C++标准库
- **版本**: C++17
- **用途**: 基础数据结构和算法
- **说明**: 标准库功能，无需额外安装

---

## 二、可选依赖

### 1. Protoactor-cpp（Actor框架）
- **GitHub**: https://github.com/DanielKernel/Protoactor-cpp.git
- **用途**: Actor模型实现
- **依赖**:
  - grpc (gRPC通信库)
  - protobuf (Protocol Buffer序列化)
  - openssl (SSL/TLS安全通信)
  - zlib (数据压缩)
- **说明**:
  - 提供Actor消息机制
  - 支持分布式Actor通信
  - Node控制面Actor实现
  - 系统维护通信面基础
- **当前状态**: 项目提供stub实现，Protoactor-cpp启用时使用完整功能
- **C++标准**: C++17

### 2. OpenSSL（SSL/TLS加密）
- **官网**: https://www.openssl.org/
- **用途**: SSL/TLS安全通信
- **说明**:
  - 提供SSL/TLS协议支持
  - 证书管理
  - 加密通信
- **当前状态**: 项目提供stub实现，OpenSSL启用时使用完整功能

### 3. libwebsockets（WebSocket支持）
- **官网**: https://libwebsockets.org/
- **用途**: WebSocket协议实现
- **说明**:
  - WebSocket服务器和客户端
  - 实时双向通信
- **当前状态**: 项目提供stub实现，libwebsockets启用时使用完整功能

### 4. libevent（事件循环）
- **官网**: https://libevent.org/
- **用途**: 高性能事件循环
- **说明**:
  - 跨平台事件循环
  - 高性能I/O多路复用
- **当前状态**: 项目提供stub实现，libevent启用时使用完整功能

### 5. RapidJSON（JSON解析）
- **GitHub**: https://github.com/Tencent/rapidjson
- **用途**: JSON序列化和反序列化
- **说明**:
  - Header-only库
  - 高性能JSON解析
- **当前状态**: 项目提供stub实现，RapidJSON启用时使用完整功能

### 6. protobuf-c / Protobuf（序列化）
- **官网**: https://developers.google.com/protocol-buffers
- **用途**: 二进制序列化协议
- **说明**:
  - 高效的二进制序列化
  - 跨语言支持
- **当前状态**: 项目提供stub实现，Protobuf启用时使用完整功能

---

## 三、依赖配置

### CMakeLists.txt配置
项目通过CMake自动检测这些依赖，如果未找到则使用stub实现：

```cmake
# Protoactor-cpp for Actor-based implementation
find_package(protoactor CONFIG QUIET)

# OpenSSL for SSL/TLS support
find_package(OpenSSL QUIET)

# libwebsockets for WebSocket support
find_package(libwebsockets CONFIG QUIET)

# protobuf for serialization support
find_package(Protobuf QUIET)

# libevent for event loop support
find_package(libevent CONFIG QUIET)

# rapidjson for JSON parsing (header-only)
if(NOT DEFINED RAPIDJSON_INCLUDE_DIR)
  find_path(RAPIDJSON_INCLUDE_DIR NAMES rapidjson/document.h)
endif()
```

### 安装依赖

**Ubuntu/Debian**:
```bash
# Protoactor-cpp依赖
sudo apt-get install -y \
    libgrpc++-dev \
    libgrpc-dev \
    libprotobuf-dev \
    protobuf-compiler \
    libssl-dev \
    zlib1g-dev

# Protoactor-cpp（从源码编译）
git clone https://github.com/DanielKernel/Protoactor-cpp.git
cd Protoactor-cpp && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc) && sudo make install

# OpenSSL（AgentFlow使用）
sudo apt-get install libssl-dev

# libwebsockets
sudo apt-get install libwebsockets-dev

# libevent
sudo apt-get install libevent-dev

# Protobuf（AgentFlow使用）
sudo apt-get install libprotobuf-dev protobuf-compiler

# RapidJSON（header-only，手动下载或通过包管理器）
sudo apt-get install rapidjson-dev
```

**macOS**:
```bash
# Protoactor-cpp依赖
brew install grpc protobuf openssl zlib

# Protoactor-cpp（从源码编译）
git clone https://github.com/DanielKernel/Protoactor-cpp.git
cd Protoactor-cpp && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(sysctl -n hw.ncpu) && sudo make install

# 其他AgentFlow依赖
brew install libwebsockets libevent rapidjson
```

---

## 四、Stub实现说明

对于每个可选依赖，AgentFlow都提供了stub实现：

- **作用**: 在依赖库未安装时提供基本功能
- **限制**: 功能受限，仅用于测试和基本场景
- **使用**: CMake自动检测依赖，未找到时使用stub

### Stub实现列表

1. **Actor Stub**: `src/actor/stub_impl.cpp`
2. **SSL Stub**: `include/agentflow/network/ssl_adapter/openssl_impl.h`
3. **WebSocket Stub**: `include/agentflow/network/websocket_adapter/libwebsockets_impl.h`
4. **Event Loop Stub**: `include/agentflow/event/eventloop_adapter/libevent_impl.h`
5. **JSON Stub**: `src/dsl/json_adapter_stub.cpp`
6. **Protobuf Stub**: `include/agentflow/serialization/proto_adapter/protobuf_impl.h`

---

## 五、适配器模式

AgentFlow使用适配器模式隔离第三方库依赖：

```cpp
// 接口定义（项目内部）
class ISslAdapter { ... };

// 具体实现（第三方库）
class SslOpensslAdapter : public ISslAdapter { ... };

// Stub实现（降级方案）
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

## 六、注意事项

1. **不要重新实现这些库**: 这些第三方库都有成熟实现，不需要在AgentFlow中重新实现

2. **依赖冲突**: 确保所有依赖的版本兼容

3. **编译顺序**: Protoactor-cpp需要先编译，其他库可以通过包管理器安装

4. **生产环境**: 生产环境建议启用所有依赖，使用完整功能而非stub

5. **许可证**: 这些库都有自己的开源许可证，使用时请遵守相应协议

---

## 七、Protoactor-cpp依赖详解

Protoactor-cpp是一个基于Actor模型的C++框架，实现分布式Actor通信和消息机制。以下是详细的依赖说明：

### 核心依赖

| 依赖 | 版本要求 | 用途 |
|------|---------|------|
| grpc | >= 1.30 | gRPC通信库，用于远程Actor通信 |
| protobuf | >= 3.10 | Protocol Buffer，用于消息序列化 |
| openssl | >= 1.1 | SSL/TLS加密，安全通信支持 |
| zlib | >= 1.2 | 数据压缩，减少网络传输数据量 |

### 编译要求

- **C++标准**: C++17
- **CMake版本**: >= 3.10
- **编译器**: GCC 7+, Clang 5+, MSVC 2017+

### 支持的平台

- Linux (x86_64, ARM64)
- macOS (x86_64, Apple Silicon ARM64)
- Windows (x86_64, CMake支持)

### 安装方式

**方式一：从源码编译（推荐）**

```bash
# 克隆仓库
git clone https://github.com/DanielKernel/Protoactor-cpp.git
cd Protoactor-cpp

# 创建构建目录
mkdir build && cd build

# 配置构建（可选择安装路径）
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local

# 编译并安装
make -j$(nproc)
sudo make install

# 配置环境变量（如需要）
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH
```

**方式二：指定自定义安装路径**

```bash
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/.local
make -j$(nproc)
make install

# 配置环境
export PATH=$HOME/.local/bin:$PATH
export LD_LIBRARY_PATH=$HOME/.local/lib:$LD_LIBRARY_PATH
export PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig:$PKG_CONFIG_PATH
```

### 验证安装

```bash
# 检查库文件
ls -l /usr/local/lib/libprotoactor*

# 检查头文件
ls -l /usr/local/include/protoactor/

# 检查CMake配置
cmake --find-package -DNAME=protoactor -DCOMPILER_ID=GNU -DLANGUAGE=CXX -DMODE=EXIST
```

### 故障排除

**问题1：找不到grpc相关库**

```bash
# Ubuntu/Debian
sudo apt-get install libgrpc++-dev libgrpc-dev

# CentOS/RHEL
sudo yum install grpc-devel grpc-plugins

# macOS
brew install grpc
```

**问题2：protobuf版本冲突**

```bash
# 卸载旧版本后重新安装
sudo apt-get remove libprotobuf-dev protobuf-compiler
sudo apt-get install libprotobuf-dev protobuf-compiler
```

**问题3：openssl找不到**

```bash
# Ubuntu/Debian
sudo apt-get install libssl-dev

# macOS
brew install openssl
export OPENSSL_ROOT_DIR=$(brew --prefix openssl)
```

**问题4：zlib缺失**

```bash
# Ubuntu/Debian
sudo apt-get install zlib1g-dev

# CentOS/RHEL
sudo yum install zlib-devel

# macOS
brew install zlib
```

---

## 八、更新日志

| 日期 | 变更 |
|------|------|
| 2026-02-17 | 更新C++标准至C++17，补充Protoactor-cpp完整依赖说明 |
| 2026-02-16 | 初始版本，列出所有外部依赖 |
