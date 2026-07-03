#!/bin/bash

set -euo pipefail

AR_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AR_PROJECT_ROOT="$(cd "$AR_SCRIPT_DIR/.." && pwd)"

ar_detect_cores() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.logicalcpu 2>/dev/null || echo 4
    else
        echo 4
    fi
}

ar_load_env() {
    local env_file="${1:-$AR_SCRIPT_DIR/.env}"
    if [ -f "$env_file" ]; then
        set -a
        # shellcheck disable=SC1090
        source "$env_file"
        set +a
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
