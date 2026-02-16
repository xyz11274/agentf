# Protoactor-cpp 安装指南

AgentFlow项目依赖Protoactor-cpp框架实现Actor模型。

## 快速安装

### 1. 克隆Protoactor-cpp项目

```bash
git clone https://github.com/DanielKernel/Protoactor-cpp.git
cd Protoactor-cpp
```

### 2. 安装系统依赖

Protoactor-cpp依赖以下库：
- grpc (gRPC通信)
- protobuf (Protocol Buffer)
- openssl (安全通信)
- zlib (压缩)

**Ubuntu/Debian系统**：
```bash
sudo apt-get install -y \
    libgrpc++-dev \
    libgrpc-dev \
    libprotobuf-dev \
    protobuf-compiler \
    libssl-dev \
    zlib1g-dev \
    python3-grpcio-tools
```

**CentOS/RHEL系统**：
```bash
sudo yum install -y \
    grpc-devel \
    protobuf-devel \
    openssl-devel \
    zlib-devel
```

### 3. 构建和安装Protoactor-cpp

```bash
cd Protoactor-cpp
mkdir build
cd build
cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local
make -j$(nproc)
sudo make install
```

### 4. 验证安装

```bash
pkg-config --cflags --libs protoactor
```

应该看到类似输出：
```
-I/usr/local/include -L/usr/local/lib -lprotoactor
```

## AgentFlow 中使用Protoactor-cpp

AgentFlow的构建系统会自动查找已安装的Protoactor-cpp。

如果Protoactor-cpp安装在非标准位置，可以在构建AgentFlow时指定：

```bash
mkdir build
cd build
cmake .. -DCMAKE_PREFIX_PATH=/path/to/protoactor/install
make
```

## 替代方案：使用Conan包管理器

如果您使用Conan包管理器：

```bash
# 在Protoactor-cpp的conanfile.txt中是否包含protoactor包声明
# 如果有，可以使用 conan 安装
conan install . --install-folder=build
```

## 故障排除

### pkg-config找不到protoactor

确保已正确安装Protoactor-cpp并设置了PKG_CONFIG_PATH：

```bash
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
```

### CMake找不到Protoactor

检查CMake的find_package能否找到：

```bash
cmake-gui /workspaces/AgentFlow
# 或者增加调试信息
cmake .. -DCMAKE_MESSAGE_LOG_LEVEL=DEBUG
```

## 支持的版本

- Protoactor-cpp: >= 1.0.0
- C++: C++11 或更高
- 编译器: GCC 5+, Clang 3.8+

## 更多信息

- [Protoactor-cpp GitHub](https://github.com/DanielKernel/Protoactor-cpp)
- [Protoactor模式说明](https://proto.actor/)
