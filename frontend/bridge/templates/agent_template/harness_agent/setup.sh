#!/bin/bash
# Harness Agent 环境初始化
#
# 做三件事：
#   1. 创建 Python 虚拟环境 .venv
#   2. 安装 requirements.txt 依赖
#   3. 提示如何用 PYTHON_PATH 启动 daemon（框架 worker 用 .venv 的 python3 执行节点脚本）
#
# 用法：
#   bash setup.sh                  # 初始化环境
#   PYTHON_PATH=.venv ./build/agentflowd --listen 0.0.0.0:7891 --package <agent_package_dir>
#
# 环境变量（可从 .env 加载）：
#   API_KEY             — LLM API key
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

# ── 3. 校验 .env ──
if [ ! -f "$SCRIPT_DIR/.env" ]; then
    echo ""
    echo "⚠️  未找到 .env 文件"
    echo "   请复制 .env.example 并填入真实 API key："
    echo "   cp .env.example .env && \$EDITOR .env"
fi

# ── 4. 输出启动指引 ──
cat <<EOF

✅ 环境初始化完成

启动 daemon（在项目根目录执行）：

  # 加载 .env 环境变量
  set -a; . <agent_package_dir>/.env; set +a

  # 用 .venv 的 python3 执行节点脚本
  PYTHON_PATH=<agent_package_dir>/.venv \\
    ./build/agentflowd \\
    --listen 0.0.0.0:7891 \\
    --package <agent_package_dir>

关键点：
  - PYTHON_PATH 指向 .venv 目录（框架 worker 会自动找 .venv/bin/python3）
  - .env 里的业务 API key 会被 export 到 worker 子进程
EOF
