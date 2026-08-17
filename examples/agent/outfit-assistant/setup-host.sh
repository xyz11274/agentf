#!/bin/bash
# Harness Agent 宿主环境初始化（setup-host.sh）
#
# 定位（对齐 docs/design/agent-template-delta.md §5.4）：由 Bridge 在 create/update 后驱动执行，
# 用户无需手动跑。负责宿主机侧的 .venv + 依赖 + agentflow-runtime + sentinel。
# 与 setup.sh（沙箱版）分离：本脚本只在宿主执行，不写沙箱 profile。
#
# 做四件事：
#   1. 创建 Python 虚拟环境 .venv
#   2. 安装 requirements.txt 依赖
#   3. 安装内部库 agentflow-runtime（editable mode）
#   4. 写 sentinel 文件（shell wrapper 检测 .venv 就绪时读这个文件，见 §3.5）
#
# 用法：
#   bash setup-host.sh             # 宿主环境初始化（由 Bridge 驱动，也可手动跑）
#   ./build/agentflowd --listen 0.0.0.0:7891 --package <agent_package_dir>
#   （无需设 PYTHON_PATH；shell wrapper nodes/*.sh 自动检测 .venv 并 fallback 系统 python3）
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

# ── 3. 安装内部库 agentflow-runtime ──
# 内部库从本地路径安装，不走公网（editable mode，源码改动即时生效）
AF_RUNTIME_PKG="$SCRIPT_DIR/../../../packages/agentflow-runtime"
AF_SENTINEL="$VENV_DIR/.agentflow_installed"
if [ -d "$AF_RUNTIME_PKG" ]; then
    "$VENV_DIR/bin/pip" install -e "$AF_RUNTIME_PKG" --quiet 2>/dev/null \
        && touch "$AF_SENTINEL" \
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

✅ 宿主环境初始化完成

启动 daemon（在项目根目录执行）：

  # 加载 .env 环境变量（QWeather / LLM key）
  set -a; . <agent_package_dir>/.env; set +a

  # shell wrapper（nodes/*.sh）自动检测 .venv 并选 python，无需设 PYTHON_PATH
  ./build/agentflowd \\
    --listen 0.0.0.0:7891 \\
    --package <agent_package_dir>

关键点：
  - workflow 用 execution_mode=shell_script，daemon execve bash nodes/*.sh
  - shell wrapper 检测 .venv + sentinel（见 §3.5）：就绪 → 用 .venv/bin/python3，否则 fallback 系统 python3
  - 系统 python3 须已预装 agentflow-runtime（由 Bridge 启动时预装，见 §2.2）
  - .env 里的业务 API key 会被 export 到 worker 子进程
EOF
