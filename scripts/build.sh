#!/bin/bash
set -e

# ============================================================
# build.sh — AgentFlow 编译脚本
# 用法: ./ar_scripts/build.sh [target]
#       不指定 target 则编译全部
#       指定 target 则仅编译该目标（如 agent_runner / agentflowd）
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$PROJECT_DIR"

echo "=== AgentFlow Build ==="
echo "Project dir: $PROJECT_DIR"
echo "Target: ${1:-all}"
echo ""

BUILD_DIR="$PROJECT_DIR/build"
if [ ! -d "$BUILD_DIR" ]; then
    echo "[INFO] Creating build directory..."
    cmake -B build -DCMAKE_BUILD_TYPE=Release -DAGENTFLOW_WITH_RAPIDJSON=ON 2>&1
fi

if command -v nproc &> /dev/null; then
    NUM_CORES=$(nproc)
else
    NUM_CORES=$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)
fi

if [ -n "$1" ]; then
    echo "[BUILD] Building target: $1 (cores=$NUM_CORES)"
    cmake --build build --target "$1" -j"$NUM_CORES" 2>&1
else
    echo "[BUILD] Building all targets... (cores=$NUM_CORES)"
    cmake --build build -j"$NUM_CORES" 2>&1
fi

echo ""
echo "[DONE] Build completed successfully."
echo ""
echo "Available binaries:"
ls -lh "$BUILD_DIR/agent_runner" "$BUILD_DIR/agentflowd" 2>/dev/null || true
