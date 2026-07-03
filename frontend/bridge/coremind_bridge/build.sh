#!/bin/bash
# Build the CoreMind Bridge C++ sidecar.
# Runs in Huawei CI/CD environment where CoreMind .so + headers are available.
#
# Usage:
#   # In HealthAgent CI/CD container:
#   bash frontend/bridge/coremind_bridge/build.sh
#
#   # Or specify platform dir explicitly:
#   PLATFORM_DIR=/opt/coremind/platform bash frontend/bridge/coremind_bridge/build.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(realpath "$SCRIPT_DIR/../../..")"

# ── Platform dir (CoreMind .so + headers) ──
# In HealthAgent CI/CD, platform is at HealthAgent/build/platform
# Default: search relative paths
if [ -z "$PLATFORM_DIR" ]; then
    CANDIDATES=(
        "$PROJECT_ROOT/../../health/build/platform"
        "/opt/coremind/platform"
        "/opt/coremind/build/platform"
    )
    for dir in "${CANDIDATES[@]}"; do
        if [ -d "$dir" ] && [ -f "$dir/lib/libcm_framework_om.so" ]; then
            PLATFORM_DIR="$dir"
            break
        fi
    done
fi

if [ -z "$PLATFORM_DIR" ] || [ ! -f "$PLATFORM_DIR/lib/libcm_framework_om.so" ]; then
    echo "❌ CoreMind platform not found. Set PLATFORM_DIR or run in HealthAgent CI/CD."
    echo "   Expected: \$PLATFORM_DIR/lib/libcm_framework_om.so"
    exit 1
fi

echo "✅ CoreMind platform: $PLATFORM_DIR"

# ── Build ──
BUILD_DIR="${SCRIPT_DIR}/build"
mkdir -p "$BUILD_DIR"

cmake -S "$SCRIPT_DIR" \
      -B "$BUILD_DIR" \
      -DPLATFORM_DIR="$PLATFORM_DIR" \
      -DCMAKE_BUILD_TYPE=Release

cmake --build "$BUILD_DIR" -- -j$(nproc)

echo ""
echo "✅ Build complete: ${BUILD_DIR}/coremind_bridge"
ls -lh "${BUILD_DIR}/coremind_bridge"
