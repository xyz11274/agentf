#!/bin/bash
# Unified AgentFlow build entrypoint.
#
# Usage:
#   ./scripts/build.sh                         # release, all targets
#   ./scripts/build.sh agentflowd              # compatibility: release target
#   ./scripts/build.sh dev [target] [--clean]
#   ./scripts/build.sh release [target] [--clean]
#   PLATFORM_DIR=/opt/coremind/platform ./scripts/build.sh production [--clean]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

# 加载环境配置（AF_BUILD_PROFILE 等）
ar_load_env

# 默认使用 production profile，可通过 AF_BUILD_PROFILE 环境变量覆盖
PROFILE="${AF_BUILD_PROFILE:-production}"
TARGET=""
CLEAN=false
DRY_RUN=false

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    sed -n '2,10p' "$0"
    exit 0
fi

case "${1:-}" in
    dev|release|production)
        PROFILE="$1"
        shift
        ;;
    ""|--clean|--dry-run) ;;
    *)
        TARGET="$1"
        shift
        ;;
esac

for arg in "$@"; do
    case "$arg" in
        --clean) CLEAN=true ;;
        --dry-run) DRY_RUN=true ;;
        -h|--help)
            sed -n '2,10p' "$0"
            exit 0
            ;;
        *)
            if [ -z "$TARGET" ]; then TARGET="$arg"
            else echo "[ERROR] Unexpected argument: $arg" >&2; exit 2
            fi
            ;;
    esac
done

case "$PROFILE" in
    dev)
        BUILD_TYPE="Debug"
        BUILD_DIR="$AR_PROJECT_ROOT/build/dev"
        ;;
    release)
        BUILD_TYPE="Release"
        BUILD_DIR="$AR_PROJECT_ROOT/build"
        ;;
    production)
        BUILD_TYPE="Release"
        BUILD_DIR="$AR_PROJECT_ROOT/build/production"
        ;;
esac

CORES="$(ar_detect_cores)"
echo "=== AgentFlow Build ==="
echo "  profile: $PROFILE"
echo "  type:    $BUILD_TYPE"
echo "  output:  $BUILD_DIR"
echo "  target:  ${TARGET:-all}"
echo "  cores:   $CORES"

if $DRY_RUN; then
    if [ "$PROFILE" = "production" ]; then
        echo "  sidecar: production CoreMind build"
        echo "  platform: ${PLATFORM_DIR:-<required>}"
    fi
    exit 0
fi

ar_require_command cmake

# 自动推断 PLATFORM_DIR（CoreMind SDK 路径）
# 从 HealthAgent 的 CMakeLists.txt 可知 SDK 在 HealthAgent/platform/ 下。
# 常见的 CI/CD 部署路径如下：
if [ "$PROFILE" = "production" ] && [ -z "${PLATFORM_DIR:-}" ]; then
    CANDIDATES=(
        "$AR_PROJECT_ROOT/../HealthAgent/platform"
        "$AR_PROJECT_ROOT/../health/build/platform"
        "/opt/coremind/platform"
        "/opt/coremind/build/platform"
    )
    for dir in "${CANDIDATES[@]}"; do
        if [ -d "$dir" ] && [ -f "$dir/lib/libcm_framework_om.so" ]; then
            PLATFORM_DIR="$dir"
            echo "  platform: auto-detected $PLATFORM_DIR"
            break
        fi
    done
fi

if [ "$PROFILE" = "production" ] && [ -z "${PLATFORM_DIR:-}" ]; then
    echo "[ERROR] production build requires PLATFORM_DIR for CoreMind libraries" >&2
    echo "  SDK should be at HealthAgent/platform/ or /opt/coremind/platform/" >&2
    echo "  Set PLATFORM_DIR explicitly or deploy the SDK to one of the above paths." >&2
    exit 1
fi
if $CLEAN; then
    rm -rf "$BUILD_DIR"
    # 清理所有 profile 的 sidecar build（production real SDK + dev/release stub）
    rm -rf "$AR_PROJECT_ROOT/sidecar/build-production"
    rm -rf "$AR_PROJECT_ROOT/sidecar/build-dev"
    rm -rf "$AR_PROJECT_ROOT/sidecar/build-release"
fi

CMAKE_COMMAND=(
    cmake -S "$AR_PROJECT_ROOT" -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    -DAGENTFLOW_WITH_RAPIDJSON=ON
)
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ] && command -v ninja >/dev/null 2>&1; then
    CMAKE_COMMAND+=(-G Ninja)
fi
"${CMAKE_COMMAND[@]}"

BUILD_COMMAND=(cmake --build "$BUILD_DIR" --parallel "$CORES")
if [ -n "$TARGET" ]; then
    BUILD_COMMAND+=(--target "$TARGET")
fi
"${BUILD_COMMAND[@]}"

if [ "$PROFILE" = "production" ]; then
    SIDECAR_BUILD="$AR_PROJECT_ROOT/sidecar/build-production"
    cmake -S "$AR_PROJECT_ROOT/sidecar" \
        -B "$SIDECAR_BUILD" \
        -DCOREMIND_ENABLED=ON \
        -DPLATFORM_DIR="$PLATFORM_DIR" \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "$SIDECAR_BUILD" --parallel "$CORES"
    echo "  daemon:  $BUILD_DIR/agentflowd"
    echo "  sidecar: $SIDECAR_BUILD/coremind_bridge (COREMIND_ENABLED=ON, real SDK)"
elif [ "$PROFILE" = "dev" ] || [ "$PROFILE" = "release" ]; then
    # 本地无 CoreMind C++ SDK 时构建 stub 模式 sidecar：从 $COREMIND_STUB_SERVICES
    # 解析伪实例，无需任何专有库即可跑完整 E2E。详见
    # frontend/bridge/docs/design/architecture/modules/coremind-bridge.md "构建与启动"
    SIDECAR_BUILD="$AR_PROJECT_ROOT/sidecar/build-$PROFILE"
    cmake -S "$AR_PROJECT_ROOT/sidecar" \
        -B "$SIDECAR_BUILD" \
        -DCOREMIND_ENABLED=OFF \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    cmake --build "$SIDECAR_BUILD" --parallel "$CORES"
    echo "  daemon:  $BUILD_DIR/agentflowd"
    echo "  sidecar: $SIDECAR_BUILD/coremind_bridge (COREMIND_ENABLED=OFF, stub mode)"
fi

echo "[DONE] $PROFILE build completed"
