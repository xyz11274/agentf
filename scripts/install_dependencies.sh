#!/bin/bash

# AgentFlow 依赖安装脚本
# 用于安装编译 AgentFlow 所需的所有依赖

set -e

echo "=========================================="
echo "AgentFlow 依赖安装脚本"
echo "=========================================="
echo ""

# 检测操作系统
OS="$(uname -s)"
echo "检测到操作系统: $OS"
echo ""

# 检测包管理器
PACKAGE_MANAGER=""
if [ "$OS" = "Linux" ]; then
  if command -v apt-get &> /dev/null; then
    PACKAGE_MANAGER="apt"
  elif command -v yum &> /dev/null; then
    PACKAGE_MANAGER="yum"
  elif command -v pacman &> /dev/null; then
    PACKAGE_MANAGER="pacman"
  elif command -v dnf &> /dev/null; then
    PACKAGE_MANAGER="dnf"
  elif command -v zypper &> /dev/null; then
    PACKAGE_MANAGER="zypper"
  else
    echo "无法检测到包管理器"
    exit 1
  fi
fi

# 安装函数
install_macos() {
    echo "检查 Homebrew..."
    if ! command -v brew &> /dev/null; then
        echo "Homebrew 未安装，正在安装..."
        /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    else
        echo "Homebrew 已安装"
    fi
    echo ""

    # 安装依赖
    echo "安装依赖..."

    # OpenSSL
    echo "  - OpenSSL..."
    brew install openssl || brew upgrade openssl || true

    # Protobuf
    echo "  - Protobuf..."
    brew install protobuf || brew upgrade protobuf || true

    # libevent
    echo "  - libevent..."
    brew install libevent || brew upgrade libevent || true

    # RapidJSON
    echo "  - RapidJSON..."
    brew install rapidjson || brew upgrade rapidjson || true

    # libwebsockets
    echo "  - libwebsockets..."
    brew install libwebsockets || brew upgrade libwebsockets || true

    # Google Test
    echo "  - Google Test..."
    brew install googletest || brew upgrade googletest || true

    echo ""
    echo "macOS 依赖安装完成！"
    echo ""
    echo "Protoactor-cpp 需要从源码编译，请运行："
    echo "  mkdir -p third_party"
    echo "  cd third_party"
    echo "  git clone https://github.com/DanielKernel/Protoactor-cpp.git"
    echo "  cd Protoactor-cpp"
    echo "  mkdir build && cd build"
    echo "  cmake .."
    echo "  make"
    echo "  sudo make install"
    echo ""
}

install_ubuntu() {
    echo "更新包列表..."
    sudo apt-get update

    echo "安装依赖..."

    # OpenSSL
    echo "  - OpenSSL..."
    sudo apt-get install -y libssl-dev

    # Protobuf
    echo "  - Protobuf..."
    sudo apt-get install -y libprotobuf-dev protobuf-compiler

    # libevent
    echo "  - libevent..."
    sudo apt-get install -y libevent-dev

    # RapidJSON
    echo "  - RapidJSON..."
    sudo apt-get install -y rapidjson-dev

    # libwebsockets
    echo "  - libwebsockets..."
    sudo apt-get install -y libwebsockets-dev

    # Google Test
    echo "  - Google Test..."
    sudo apt-get install -y libgtest-dev cmake

    # pkg-config (可能需要)
    echo "  - pkg-config..."
    sudo apt-get install -y pkg-config || true

    echo ""
    echo "Ubuntu 依赖安装完成！"
    echo ""
    echo "Protoactor-cpp 需要从源码编译，请运行："
    echo "  mkdir -p third_party"
    echo "  cd third_party"
    echo "  git clone https://github.com/DanielKernel/Protoactor-cpp.git"
    echo "  cd Protoactor-cpp"
    echo "  mkdir build && cd build"
    echo "  cmake .."
    echo "  make"
    echo "  sudo make install"
    echo ""
}

install_fedora() {
    echo "更新包列表..."
    sudo dnf check-update || sudo yum check-update || true

    echo "安装依赖..."

    # OpenSSL
    echo "  - OpenSSL..."
    sudo dnf install -y openssl-devel || sudo yum install -y openssl-devel

    # Protobuf
    echo "  - Protobuf..."
    sudo dnf install -y protobuf-devel protobuf-compiler || sudo yum install -y protobuf-devel protobuf-compiler

    # libevent
    echo "  - libevent..."
    sudo dnf install -y libevent-devel || sudo yum install -y libevent-devel

    # RapidJSON
    echo "  - RapidJSON..."
    sudo dnf install -y rapidjson-devel || sudo yum install -y rapidjson-devel

    # libwebsockets
    echo "  - libwebsockets..."
    sudo dnf install -y libwebsockets-devel || sudo yum install -y libwebsockets-devel

    # Google Test
    echo "  - Google Test..."
    sudo dnf install -y gtest-devel cmake || sudo yum install -y gtest-devel cmake

    echo ""
    echo "Fedora/CentOS 依赖安装完成！"
    echo ""
}

install_arch() {
    echo "更新包列表..."
    sudo pacman -Sy

    echo "安装依赖..."

    # OpenSSL
    echo "  - OpenSSL..."
    sudo pacman -S --noconfirm openssl || true

    # Protobuf
    echo "  - Protobuf..."
    sudo pacman -S --noconfirm protobuf || true

    # libevent
    echo "  - libevent..."
    sudo pacman -S --noconfirm libevent || true

    # RapidJSON
    echo "  - RapidJSON..."
    sudo pacman -S --noconfirm rapidjson || true

    # libwebsockets
    echo "  - libwebsockets..."
    sudo pacman -S --noconfirm libwebsockets || true

    # Google Test
    echo "  - Google Test..."
    sudo pacman -S --noconfirm gtest cmake || true

    echo ""
    echo "Arch Linux 依赖安装完成！"
    echo ""
}

install_opensuse() {
    echo "更新包列表..."
    sudo zypper refresh

    echo "安装依赖..."

    # OpenSSL
    echo "  - OpenSSL..."
    sudo zypper install -y libopenssl-devel

    # Protobuf
    echo "  - Protobuf..."
    sudo zypper install -y protobuf-devel

    # libevent
    echo "  - libevent..."
    sudo zypper install -y libevent-devel

    # RapidJSON
    echo "  - RapidJSON..."
    sudo zypper install -y rapidjson-devel

    # libwebsockets
    echo "  - libwebsockets..."
    sudo zypper install -y libwebsockets-devel

    # Google Test
    echo "  - Google Test..."
    sudo zypper install -y gtest-devel cmake

    echo ""
    echo "openSUSE 依赖安装完成！"
    echo ""
}

# 根据操作系统选择安装方法
case "$OS" in
    Darwin*)
        install_macos
        ;;
    Linux*)
        case "$PACKAGE_MANAGER" in
            apt)
                install_ubuntu
                ;;
            yum|dnf)
                install_fedora
                ;;
            pacman)
                install_arch
                ;;
            zypper)
                install_opensuse
                ;;
            *)
                echo "不支持的包管理器: $PACKAGE_MANAGER"
                exit 1
                ;;
        esac
        ;;
    *)
        echo "不支持的操作系统: $OS"
        exit 1
        ;;
esac

echo "=========================================="
echo "依赖安装完成！"
echo "=========================================="
echo ""
echo "下一步："
echo "  cd /path/to/AgentFlow"
echo "  mkdir build && cd build"
echo "  cmake .."
echo "  make"
echo ""
