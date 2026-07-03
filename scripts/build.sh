#!/bin/bash
# Unified AgentFlow build entrypoint.
#
# Usage:
#   ./ar_scripts/build.sh                         # release, all targets
#   ./ar_scripts/build.sh agentflowd              # compatibility: release target
#   ./ar_scripts/build.sh dev [target] [--clean]
#   ./ar_scripts/build.sh release [target] [--clean]
#   PLATFORM_DIR=/opt/coremind/platform ./ar_scripts/build.sh production [--clean]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"
ar_load_env

PROFILE="release"
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
if [ "$PROFILE" = "production" ] && [ -z "${PLATFORM_DIR:-}" ]; then
    echo "[ERROR] production build requires PLATFORM_DIR for CoreMind libraries" >&2
    exit 1
fi
if $CLEAN; then
    rm -rf "$BUILD_DIR"
    if [ "$PROFILE" = "production" ]; then
        rm -rf "$AR_PROJECT_ROOT/frontend/bridge/coremind_bridge/build-production"
    fi
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
    SIDECAR_BUILD="$AR_PROJECT_ROOT/frontend/bridge/coremind_bridge/build-production"
    cmake -S "$AR_PROJECT_ROOT/frontend/bridge/coremind_bridge" \
        -B "$SIDECAR_BUILD" \
        -DCOREMIND_ENABLED=ON \
        -DPLATFORM_DIR="$PLATFORM_DIR" \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "$SIDECAR_BUILD" --parallel "$CORES"
    echo "  daemon:  $BUILD_DIR/agentflowd"
    echo "  sidecar: $SIDECAR_BUILD/coremind_bridge"
fi

echo "[DONE] $PROFILE build completed"
