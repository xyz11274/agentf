# AgentFlow 外部依赖说明

本文档列出了AgentFlow项目的外部依赖库，这些库由第三方实现，不需要在AgentFlow项目中重新实现。

---

## 一、核心依赖（必需）

### 1. C++标准库
- **版本**: C++11
- **用途**: 基础数据结构和算法
- **说明**: 标准库功能，无需额外安装

---

## 二、可选依赖

### 1. Protoactor-cpp（Actor框架）
- **GitHub**: https://github.com/DanielKernel/Protoactor-cpp.git
- **用途**: Actor模型实现
- **说明**:
  - 提供Actor消息机制
  - 支持分布式Actor通信
  - Node控制面Actor实现
- **当前状态**: 项目提供stub实现，Protoactor-cpp启用时使用完整功能

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
# Protoactor-cpp（需要从源码编译）
git clone https://github.com/DanielKernel/Protoactor-cpp.git
cd Protoactor-cpp && mkdir build && cd build
cmake .. && make && sudo make install

# OpenSSL
sudo apt-get install libssl-dev

# libwebsockets
sudo apt-get install libwebsockets-dev

# libevent
sudo apt-get install libevent-dev

# Protobuf
sudo apt-get install libprotobuf-dev protobuf-compiler

# RapidJSON（header-only，手动下载或通过包管理器）
sudo apt-get install rapidjson-dev
```

**macOS**:
```bash
# 使用Homebrew
brew install openssl libwebsockets libevent protobuf rapidjson

# Protoactor-cpp需要从源码编译
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

## 七、更新日志

| 日期 | 变更 |
|------|------|
| 2026-02-16 | 初始版本，列出所有外部依赖 |
