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
- grpc (gRPC通信, >= 1.30)
- protobuf (Protocol Buffer, >= 3.10)
- openssl (SSL/TLS安全通信, >= 1.1)
- zlib (数据压缩, >= 1.2)

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

**macOS系统**（使用Homebrew）：
```bash
# 安装依赖
brew install grpc protobuf openssl zlib

# OpenSSL可能需要特殊设置
export OPENSSL_ROOT_DIR=$(brew --prefix openssl)
```

**Fedora系统**：
```bash
sudo dnf install -y \
    grpc-devel \
    protobuf-devel \
    openssl-devel \
    zlib-devel
```

**Arch Linux**：
```bash
sudo pacman -S --noconfirm \
    grpc \
    protobuf \
    openssl \
    zlib
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
- C++: C++17 或更高
- 编译器: GCC 7+, Clang 5+, MSVC 2017+
- CMake: >= 3.10

## 支持的平台

- Linux (x86_64, ARM64)
- macOS (x86_64, Apple Silicon ARM64)
- Windows (x86_64, CMake支持)

## 更多信息

- [Protoactor-cpp GitHub](https://github.com/DanielKernel/Protoactor-cpp)
- [Protoactor模式说明](https://proto.actor/)

---

## 高级安装选项

### 自定义安装路径

```bash
cd Protoactor-cpp/build
cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/.local
make -j$(nproc)
make install

# 配置环境变量
export PATH=$HOME/.local/bin:$PATH
export LD_LIBRARY_PATH=$HOME/.local/lib:$LD_LIBRARY_PATH
export PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig:$PKG_CONFIG_PATH
```

### Debug构建

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
sudo make install
```

### Release构建（默认）

```bash
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
sudo make install
```

### 启用/禁用特定功能

```bash
# 仅编译核心功能（减少依赖）
cmake .. -DPROTOACTOR_ENABLE_EXAMPLES=OFF -DPROTOACTOR_ENABLE_TESTS=OFF

# 启用所有功能
cmake .. -DPROTOACTOR_ENABLE_EXAMPLES=ON -DPROTOACTOR_ENABLE_TESTS=ON
```

## 验证安装

### 检查库文件

```bash
# Linux/macOS
ls -l /usr/local/lib/libprotoactor*

# 或自定义安装路径
ls -l $HOME/.local/lib/libprotoactor*
```

### 检查头文件

```bash
# Linux/macOS
ls -l /usr/local/include/protoactor/

# 或自定义安装路径
ls -l $HOME/.local/include/protoactor/
```

### CMake配置检查

```bash
# 测试CMake能否找到Protoactor
cmake --find-package -DNAME=protoactor -DCOMPILER_ID=GNU -DLANGUAGE=CXX -DMODE=EXIST
```

### 编译测试程序

创建一个简单的测试程序 `test_protoactor.cpp`：

```cpp
#include <protoactor/actor.h>
#include <iostream>

int main() {
    std::cout << "Protoactor-cpp is available!" << std::endl;
    return 0;
}
```

编译并运行：

```bash
g++ -std=c++17 test_protoactor.cpp -I/usr/local/include -L/usr/local/lib -lprotoactor -o test_protoactor
./test_protoactor
```

---

## 故障排除（进阶）

### 问题：找不到gRPC库

```bash
# Ubuntu/Debian
sudo apt-get install libgrpc++-dev libgrpc-dev

# CentOS/RHEL
sudo yum install grpc-devel grpc-plugins

# macOS
brew install grpc
```

### 问题：protobuf版本不兼容

```bash
# 检查protobuf版本
protoc --version

# 升级protobuf（Ubuntu/Debian）
sudo apt-get install libprotobuf-dev protobuf-compiler

# 升级protobuf（macOS）
brew upgrade protobuf
```

### 问题：OpenSSL找不到

```bash
# macOS需要设置OpenSSL路径
export OPENSSL_ROOT_DIR=$(brew --prefix openssl)

# 在cmake时指定
cmake .. -DOPENSSL_ROOT_DIR=$(brew --prefix openssl)
```

### 问题：zlib缺失

```bash
# Ubuntu/Debian
sudo apt-get install zlib1g-dev

# CentOS/RHEL
sudo yum install zlib-devel

# macOS
brew install zlib
```

### 问题：运行时找不到共享库

```bash
# Linux - 添加库路径
sudo ldconfig /usr/local/lib

# 或临时设置
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH

# 永久设置（添加到~/.bashrc）
echo 'export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH' >> ~/.bashrc
source ~/.bashrc

# macOS - 设置DYLD_LIBRARY_PATH
export DYLD_LIBRARY_PATH=/usr/local/lib:$DYLD_LIBRARY_PATH
```
