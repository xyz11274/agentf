#!/bin/bash
# Daemon lifecycle manager — start / stop / restart agentflowd.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

# 加载环境配置
ar_load_env

# ── Helper: is PID a zombie? ──────────────────────────────────────────
_is_zombie() {
    local pid="$1"
    # macOS: ps -o state= 返回进程状态首字母，Z = zombie
    local state
    state=$(ps -o state= -p "$pid" 2>/dev/null | head -1 | tr -d ' ')
    [ "$state" = "Z" ]
}

# ── Helper: check if PID is alive and not zombie ──────────────────────
_is_alive() {
    local pid="$1"
    kill -0 "$pid" 2>/dev/null || return 1
    _is_zombie "$pid" && return 1 || return 0
}

# ── Helper: check if port is in use (any address) ─────────────────────
# Returns "PID PROCESS_NAME" if occupied, empty string if free.
_check_port() {
    local port="$1"
    lsof -i "tcp:${port}" -s TCP:LISTEN 2>/dev/null | awk 'NR>1 {print $2, $1}' || true
}

# ── Helper: resolve running PID ─────────────────────────────────────
_resolve_pid() {
    local -a pids=()
    # 优先读 PID 文件
    if [ -f /tmp/agentflowd.pid ]; then
        local pid_file
        pid_file=$(cat /tmp/agentflowd.pid)
        if _is_alive "$pid_file"; then
            pids+=("$pid_file")
        else
            rm -f /tmp/agentflowd.pid
        fi
    fi
    # 再 pgrep 全系统查（排除已死的僵尸）
    if [ ${#pids[@]} -eq 0 ]; then
        while IFS= read -r line; do
            [ -n "$line" ] && _is_alive "$line" && pids+=("$line")
        done < <(pgrep -x agentflowd 2>/dev/null || true)
    fi
    echo "${pids[*]:-}"
}

# ── Helper: stop daemon ─────────────────────────────────────────────
_stop_daemon() {
    local pids="$1"
    echo "[INFO] Stopping daemon (PID=$pids)..."
    # SIGTERM first
    kill $pids 2>/dev/null || true
    for _ in $(seq 1 10); do
        local all_dead=true
        for pid in $pids; do
            if _is_alive "$pid"; then
                all_dead=false
                break
            fi
        done
        $all_dead && break
        sleep 0.3
    done
    # SIGKILL survivors (僵尸进程 kill -9 无效，忽略)
    for pid in $pids; do
        if _is_alive "$pid"; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
    # 清理 PID 文件
    rm -f /tmp/agentflowd.pid
    echo "[INFO] Daemon stopped"
}

# ── Config file path (per environment) ─────────────────────────────
_AGENTFLOW_ENV="${AGENTFLOW_ENV:-dev}"
_CONFIG_FILE="$AR_PROJECT_ROOT/configs/$_AGENTFLOW_ENV/agentflowd.json"

# ── Parse arguments ─────────────────────────────────────────────────
ACTION="start"
# 默认加载 ③ workspace 运行实例（examples/agent/{name}/，三态模型见
# docs/design/features/agent-package/agent-package-three-state.md §2.1）。
# 启动脚本绕过 bridge 的 ②→③ 同步直接加载 ③ 实例（by-design：本地开发/生产
# 启动时 ③ 已就绪或由开发者手动同步）；改 ② 固化态后需手动 load 或 rsync 同步。
# agent 包路径可配置化：AF_AGENT_PACKAGE env（configs/{env}/.env 中设置，ar_load_env 已加载）
# > 默认 outfit-assistant。相对路径基于项目根，--package CLI 参数仍可显式覆盖。
PACKAGE="${AF_AGENT_PACKAGE:-examples/agent/outfit-assistant}"
PORT="17891"
case "$_AGENTFLOW_ENV" in
    prod) PROFILE="production" ;;
    *)    PROFILE="dev" ;;
esac
CONFIG_FILE="$_CONFIG_FILE"
KEEP_LOGS=false
BUILD_FIRST=false
BUILD_OPTION_SET=false

while [ "$#" -gt 0 ]; do
    case "$1" in
        start|stop|restart)
            ACTION="$1"; shift ;;
        --package) PACKAGE="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --profile) PROFILE="$2"; shift 2 ;;
        --config) CONFIG_FILE="$2"; shift 2 ;;
        --keep-logs) KEEP_LOGS=true; shift ;;
        --build) BUILD_FIRST=true; BUILD_OPTION_SET=true; shift ;;
        --no-build) BUILD_FIRST=false; BUILD_OPTION_SET=true; shift ;;
        --restart|-r) ACTION="restart"; shift ;;
        -h|--help)
            echo "Usage: $0 [start|stop|restart] [options]"
            echo ""
            echo "Commands:"
            echo "  start    Start daemon (default; fails if already running)"
            echo "  stop     Stop running daemon"
            echo "  restart  Stop, rebuild, then start (default: --build)"
            echo ""
            echo "Options:"
            echo "  --package PATH   Agent package directory"
            echo "  --port PORT      Listen port (default: 17891)"
            echo "  --profile PROF   Build profile: dev|release|production (default: dev)"
            echo "  --config PATH    Config file (default: configs/\$AGENTFLOW_ENV/agentflowd.json)"
            echo "  --build          Build binary first"
            echo "  --no-build       Skip build"
            echo "  --keep-logs      Keep previous log instead of archiving"
            echo "  -r, --restart    Alias for restart command"
            echo ""
            echo "Environment:"
            echo "  AGENTFLOW_ENV    dev|prod (default: dev)"
            echo "  AF_LOG_FILE      Daemon log path (default: logs/agentflowd.log)"
            exit 0 ;;
        *)
            if [ "$PACKAGE" = "examples/agent/agentflow_assistant" ]; then PACKAGE="$1"
            elif [ "$PORT" = "17891" ]; then PORT="$1"
            else echo "[ERROR] Unknown argument: $1" >&2; exit 2
            fi
            shift
            ;;
    esac
done

# restart 默认重新编译
if [ "$ACTION" = "restart" ] && [ "$BUILD_OPTION_SET" = false ]; then
    BUILD_FIRST=true
fi

# ═══════════════════════════════════════════════════════════════════
#  stop
# ═══════════════════════════════════════════════════════════════════
if [ "$ACTION" = "stop" ]; then
    PID=$(_resolve_pid)
    if [ -z "$PID" ]; then
        echo "[INFO] No running daemon found"
        exit 0
    fi
    _stop_daemon "$PID"
    exit 0
fi

# ═══════════════════════════════════════════════════════════════════
#  restart  → stop first, then fall through to start
# ═══════════════════════════════════════════════════════════════════
if [ "$ACTION" = "restart" ]; then
    PID=$(_resolve_pid)
    if [ -n "$PID" ]; then
        _stop_daemon "$PID"
    fi
    # After stopping daemon, check if port is still occupied by another process
    OCCUPIER=$(_check_port "$PORT")
    if [ -n "$OCCUPIER" ]; then
        OCCUPIER_PID=$(echo "$OCCUPIER" | awk '{print $1}')
        OCCUPIER_NAME=$(echo "$OCCUPIER" | awk '{print $2}')
        echo "[WARN] Port $PORT is occupied by PID=$OCCUPIER_PID ($OCCUPIER_NAME)"
        echo "[INFO] Attempting to free port $PORT..."
        kill "$OCCUPIER_PID" 2>/dev/null || true
        for _ in $(seq 1 5); do
            OCCUPIER=$(_check_port "$PORT")
            [ -z "$OCCUPIER" ] && break
            sleep 0.3
        done
        OCCUPIER=$(_check_port "$PORT")
        if [ -n "$OCCUPIER" ]; then
            OCCUPIER_PID=$(echo "$OCCUPIER" | awk '{print $1}')
            echo "[WARN] Process $OCCUPIER_PID did not exit gracefully, sending SIGKILL..."
            kill -9 "$OCCUPIER_PID" 2>/dev/null || true
            sleep 0.3
        fi
        # Final check
        OCCUPIER=$(_check_port "$PORT")
        if [ -n "$OCCUPIER" ]; then
            OCCUPIER_PID=$(echo "$OCCUPIER" | awk '{print $1}')
            OCCUPIER_NAME=$(echo "$OCCUPIER" | awk '{print $2}')
            echo "[ERROR] Cannot free port $PORT (PID=$OCCUPIER_PID, $OCCUPIER_NAME)"
            exit 1
        fi
        echo "[INFO] Port $PORT freed"
    fi
fi

# ═══════════════════════════════════════════════════════════════════
#  start
# ═══════════════════════════════════════════════════════════════════

# ── Guard: already running? ───────────────────────────────────────
PID=$(_resolve_pid)
if [ -n "$PID" ]; then
    echo "[WARN] Daemon is already running (PID=$PID)"
    echo "       To restart, use:  $0 restart"
    exit 1
fi

# ── Guard: port already in use by another process? ────────────────
OCCUPIER=$(_check_port "$PORT")
if [ -n "$OCCUPIER" ]; then
    OCCUPIER_PID=$(echo "$OCCUPIER" | awk '{print $1}')
    OCCUPIER_NAME=$(echo "$OCCUPIER" | awk '{print $2}')
    echo "[ERROR] Port $PORT is already in use by PID=$OCCUPIER_PID ($OCCUPIER_NAME)"
    echo "       Use '$0 restart' to force-clear the port, or kill the process manually"
    exit 1
fi

# ── Resolve binary ──────────────────────────────────────────────────
case "$PROFILE" in
    dev)
        # build.sh dev 输出到 build/dev/；优先用它（restart 构建产物），旧扁平 build/ 仅 fallback
        if [ -x "$AR_PROJECT_ROOT/build/dev/agentflowd" ]; then
            BUILD_DIR="$AR_PROJECT_ROOT/build/dev"
        else
            BUILD_DIR="$AR_PROJECT_ROOT/build"
        fi
        ;;
    release)
        BUILD_DIR="$AR_PROJECT_ROOT/build"
        ;;
    production)
        if [ -x "$AR_PROJECT_ROOT/build/production/agentflowd" ]; then
            BUILD_DIR="$AR_PROJECT_ROOT/build/production"
        else
            BUILD_DIR="$AR_PROJECT_ROOT/build"
        fi
        ;;
    *) echo "[ERROR] Invalid profile: $PROFILE" >&2; exit 2 ;;
esac

if $BUILD_FIRST; then
    "$SCRIPT_DIR/build.sh" "$PROFILE" agentflowd
fi
DAEMON="$BUILD_DIR/agentflowd"
if [ ! -x "$DAEMON" ]; then
    echo "[ERROR] Missing daemon binary: $DAEMON" >&2
    echo "        Run: ./scripts/build.sh $PROFILE agentflowd" >&2
    exit 1
fi

LOG_FILE="${AF_LOG_FILE:-$AR_PROJECT_ROOT/logs/agentflowd.log}"
ar_archive_log "$LOG_FILE" "$KEEP_LOGS"

if [[ "$PACKAGE" = /* ]]; then PACKAGE_PATH="$PACKAGE"
else PACKAGE_PATH="$AR_PROJECT_ROOT/$PACKAGE"
fi

# ── 三态模型：自动从 ② 固化态同步重建 ③ 运行实例（删 examples 不崩不变量）──
# 设计：agent-package-three-state.md §5。③ 缺失或 ② 比 ③ 新时 rsync ②→③。
# 排除运行时数据（.venv/outputs/workspace/.agentfoundry/preferences.json/manifest.jsonc），
# 保留 ③ 已有运行时数据。同步是幂等的（rsync --update 仅传新/改文件）。
_sync_2_to_3() {
    local pkg_name="${PACKAGE_PATH##*/}"
    local source_2="$AR_PROJECT_ROOT/resource/agent-packages/$pkg_name"
    local target_3="$PACKAGE_PATH"
    if [ ! -d "$source_2" ]; then
        echo "[WARN] ② 固化态不存在: $source_2（跳过同步，直接加载 ③）"
        return 0
    fi
    # 同步触发判定（2026-08-10 修复）：原 `source_2 -nt target_3` 比较顶层目录 mtime，
    # 深层 harness/tools 建文件不更新顶层目录 mtime → 新工具不同步（链路缺口①坐实）。
    # 改内容级检测：③ 缺失 或 ② 中存在比 ③ 顶层目录更新的文件 → 触发 rsync。
    # find -newer 兼容 BSD/GNU（macOS dev + Linux prod）；rsync --update 幂等，过度触发无害。
    local need_sync=0
    if [ ! -d "$target_3" ]; then
        need_sync=1
    elif find "$source_2" -type f -newer "$target_3" 2>/dev/null | head -1 | grep -q .; then
        need_sync=1
    fi
    if [ "$need_sync" = "1" ]; then
        echo "[INFO] 同步 ②→③: $source_2 → $target_3"
        mkdir -p "$target_3"
        rsync -a --delete \
            --exclude='.venv/' --exclude='outputs/' --exclude='workspace/' \
            --exclude='.agentfoundry/' --exclude='preferences.json' \
            --exclude='manifest.jsonc' --exclude='dist/' \
            "$source_2/" "$target_3/"
        echo "[INFO] 同步完成（运行时数据保留）"
    fi
}
_sync_2_to_3

# 调用方未显式指定时，在启动边界解析一次代码版本；运行时 Python 子进程只读该值。
if [ -z "${AF_CODE_REVISION:-}" ] && [ -z "${GIT_COMMIT:-}" ] \
    && command -v git >/dev/null 2>&1; then
    AF_CODE_REVISION=$(git -C "$AR_PROJECT_ROOT" rev-parse HEAD 2>/dev/null || true)
    export AF_CODE_REVISION
fi

# 同一 commit 的脏工作区可能运行完全不同的代码；只输出不可逆指纹，不输出 diff 内容。
if [ -z "${AF_CODE_DIRTY:-}" ] && command -v git >/dev/null 2>&1; then
    if git -C "$AR_PROJECT_ROOT" status --porcelain=v1 --untracked-files=all \
        2>/dev/null | grep -q .; then
        AF_CODE_DIRTY=1
    else
        AF_CODE_DIRTY=0
    fi
    export AF_CODE_DIRTY
fi
if [ -z "${AF_CODE_DIFF_SHA256:-}" ] && [ "${AF_CODE_DIRTY:-0}" = "1" ] \
    && command -v sha256sum >/dev/null 2>&1; then
    AF_CODE_DIFF_SHA256=$(
        {
            git -C "$AR_PROJECT_ROOT" diff --no-ext-diff --binary HEAD -- 2>/dev/null
            git -C "$AR_PROJECT_ROOT" status --porcelain=v1 --untracked-files=all 2>/dev/null
            git -C "$AR_PROJECT_ROOT" ls-files --others --exclude-standard 2>/dev/null \
                | git -C "$AR_PROJECT_ROOT" hash-object --stdin-paths 2>/dev/null
        } | sha256sum | awk '{print $1}'
    )
    export AF_CODE_DIFF_SHA256
fi

# ── Launch (注入 AF_LOG_FILE 确保 skill_stderr 日志落 logs/) ──────────
echo "[INFO] Starting daemon in background..."
echo "[INFO] Daemon logs → $LOG_FILE"
echo "[INFO] Config file → $CONFIG_FILE"
echo "[INFO] View live: tail -f $LOG_FILE"
export AF_LOG_FILE="$LOG_FILE"
nohup "$DAEMON" \
    --config "$CONFIG_FILE" \
    --package "$PACKAGE_PATH" \
    --listen "127.0.0.1:$PORT" \
    >> "$LOG_FILE" 2>&1 &
DAEMON_PID=$!
echo "$DAEMON_PID" > /tmp/agentflowd.pid
echo "[INFO] Daemon started (PID=$DAEMON_PID)"
echo "[INFO] Stop: $0 stop"
