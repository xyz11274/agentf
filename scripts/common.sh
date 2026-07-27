#!/bin/bash

set -euo pipefail

AR_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AR_PROJECT_ROOT="$(cd "$AR_SCRIPT_DIR/.." && pwd)"

ar_detect_cores() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif [ -f /proc/cpuinfo ]; then
        grep -c ^processor /proc/cpuinfo 2>/dev/null || echo 4
    elif command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.logicalcpu 2>/dev/null || echo 4
    else
        echo 4
    fi
}

ar_archive_log() {
    local log_file="$1"
    local keep_logs="${2:-false}"
    mkdir -p "$(dirname "$log_file")"
    if [ "$keep_logs" != "true" ] && [ -f "$log_file" ]; then
        local archived="${log_file}.$(date +%Y%m%d-%H%M%S)"
        mv "$log_file" "$archived"
        echo "[INFO] Archived previous log: $archived"
    fi
}

ar_require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "[ERROR] Required command not found: $1" >&2
        exit 1
    fi
}

# 加载环境对应的 .env 文件
# 根据 AGENTFLOW_ENV 决定加载哪个配置目录下的 .env：
#   prod  → configs/prod/.env
#   test  → configs/test/.env
#   dev   → configs/dev/.env（默认）
# 加载后，脚本可通过 $AF_BUILD_PROFILE 等变量获取环境配置。
ar_load_env() {
    local env="${AGENTFLOW_ENV:-dev}"
    local env_file="$AR_PROJECT_ROOT/configs/$env/.env"
    if [ -f "$env_file" ]; then
        set -a
        source "$env_file"
        set +a
        echo "[env] loaded $env_file" >&2
    else
        echo "[env] WARNING: $env_file not found" >&2
    fi
}
