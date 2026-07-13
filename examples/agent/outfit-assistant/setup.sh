#!/bin/bash
# OutfitAssistant 环境初始化
#
# 定位（对齐 docs/design/agent-template-delta.md §5.4）：用户手动跑一次的初始化脚本，
# 不再被任何 workflow 触发。负责创建 .venv + 装依赖 + 装 agentflow-runtime 内部库。
#
# 做三件事：
#   1. 创建 Python 虚拟环境 .venv
#   2. 安装 requirements.txt 依赖
#   3. 提示如何用 PYTHON_PATH 启动 daemon（框架 worker 用 .venv 的 python3 直接 execve 节点 .py）
#
# 用法：
#   bash setup.sh                  # 初始化环境
#   PYTHON_PATH=.venv ./build/agentflowd --listen 0.0.0.0:7891 --package examples/agent/outfit-assistant
#
# 环境变量（可从 .env 加载）：
#   API_KEY             — LLM API key
#   QWEATHER_API_KEY    — 和风天气 API key
#   QWEATHER_API_HOST   — 和风天气 API host
#   AF_LLM_BASE_URL     — LLM API base URL（可选，覆盖 agent.md）
#   AF_LLM_MODEL        — LLM 模型名（可选，覆盖 agent.md）

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

VENV_DIR="$SCRIPT_DIR/.venv"
REQ_FILE="$SCRIPT_DIR/requirements.txt"
HASH_FILE="$VENV_DIR/.requirements_hash"

# ── 1. 创建 venv ──
if [ -f "$VENV_DIR/bin/python3" ]; then
    echo "✅ .venv 已存在，跳过创建"
else
    echo "📦 创建 Python 虚拟环境 .venv ..."
    python3 -m venv "$VENV_DIR" 2>/dev/null || python3 -m venv --without-pip "$VENV_DIR"
    if [ ! -f "$VENV_DIR/bin/pip" ]; then
        "$VENV_DIR/bin/python3" -m ensurepip --upgrade --default-pip 2>/dev/null || true
    fi
fi

# ── 2. 安装依赖（requirements.txt 变化时重装）──
if [ -f "$VENV_DIR/bin/pip" ] && [ -f "$REQ_FILE" ]; then
    if command -v md5sum >/dev/null 2>&1; then
        HASH=$(md5sum "$REQ_FILE" | cut -d' ' -f1)
    else
        HASH=$(md5 -q "$REQ_FILE")   # macOS
    fi
    if [ ! -f "$HASH_FILE" ] || [ "$(cat "$HASH_FILE")" != "$HASH" ]; then
        echo "📦 安装依赖 ..."
        "$VENV_DIR/bin/pip" install -r "$REQ_FILE" --quiet
        echo "$HASH" > "$HASH_FILE"
        echo "✅ 依赖安装完成"
    else
        echo "✅ 依赖已是最新，跳过安装"
    fi
fi

# ── 3. 安装内部库 agentflow-runtime ──
# 内部库从本地路径安装，不走公网（editable mode，源码改动即时生效）
AF_RUNTIME_PKG="$SCRIPT_DIR/../../../packages/agentflow-runtime"
if [ -d "$AF_RUNTIME_PKG" ]; then
    "$VENV_DIR/bin/pip" install -e "$AF_RUNTIME_PKG" --quiet 2>/dev/null \
        && echo "✅ agentflow-runtime 已安装" \
        || echo "⚠️  agentflow-runtime 安装失败"
fi

# ── 4. 校验 .env ──
if [ ! -f "$SCRIPT_DIR/.env" ]; then
    echo ""
    echo "⚠️  未找到 .env 文件"
    echo "   请复制 .env.example 并填入真实 API key："
    echo "   cp .env.example .env && \$EDITOR .env"
fi

# ── 5. 输出启动指引 ──
cat <<EOF

✅ 环境初始化完成

启动 daemon（在项目根目录执行）：

  # 加载 .env 环境变量（QWeather / LLM key）
  set -a; . examples/agent/outfit-assistant/.env; set +a

  # 用 .venv 的 python3 直接 execve 节点 .py 入口文件
  PYTHON_PATH=examples/agent/outfit-assistant/.venv \\
    ./build/agentflowd \\
    --listen 0.0.0.0:7891 \\
    --package examples/agent/outfit-assistant

关键点：
  - PYTHON_PATH 指向 .venv 目录（框架 worker 会自动找 .venv/bin/python3）
  - 节点入口文件（nodes/*.py）只有一行 import，真正实现在 agentflow-runtime pip 包里
  - .env 里的 QWEATHER_API_KEY / QWEATHER_API_HOST 会被 export 到 worker 子进程
EOF
