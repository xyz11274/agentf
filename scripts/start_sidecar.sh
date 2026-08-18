#!/bin/bash
# Start / stop / restart the C++ CoreMind SideCar (coremind_bridge).
# Aligned with start_daemon.sh / start_bridge.sh lifecycle pattern.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

PID_FILE="/tmp/agentflow-sidecar.pid"

# ── Helper: resolve running PID ─────────────────────────────────────
_resolve_pid() {
    local pid=""
    if [ -f "$PID_FILE" ]; then
        pid=$(cat "$PID_FILE")
        if ! kill -0 "$pid" 2>/dev/null; then
            rm -f "$PID_FILE"
            pid=""
        fi
    fi
    if [ -z "$pid" ]; then
        pid=$(pgrep -x coremind_bridge 2>/dev/null || true)
    fi
    echo "$pid"
}

# ── Helper: stop sidecar ────────────────────────────────────────────
_stop_sidecar() {
    local pid="$1"
    echo "[INFO] Stopping sidecar (PID=$pid)..."
    kill "$pid" 2>/dev/null
    for _ in $(seq 1 10); do
        if ! kill -0 "$pid" 2>/dev/null; then
            break
        fi
        sleep 0.3
    done
    if kill -0 "$pid" 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$PID_FILE"
    echo "[INFO] SideCar stopped"
}

# ── Parse arguments ─────────────────────────────────────────────────
# 支持两种等价的传参方式（不可混用冲突）：
#   位置参数:  $0 [start|stop|restart] [profile] [port]     如 start_sidecar.sh local 23768
#   选项参数:  $0 [start|stop|restart] [--profile PROF] [--port PORT]
# 位置参数与选项统一在 while 中解析，避免 PROFILE/PORT 被 $1/$2 提前污染
# （旧实现中 `--profile local` 会把 PORT 置为 "local"，导致 --listen 解析失败）。
ACTION="start"
PROFILE=""
PORT=""
KEEP_LOGS=false

while [ $# -gt 0 ]; do
    case "$1" in
        start|stop|restart) ACTION="$1"; shift ;;
        --port|-p)          PORT="${2:?Missing port value}"; shift 2 ;;
        --profile)          PROFILE="$2"; shift 2 ;;
        --keep-logs)        KEEP_LOGS=true; shift ;;
        -h|--help)
            echo "Usage: $0 [start|stop|restart] [profile] [port]"
            echo ""
            echo "Commands:"
            echo "  start    Start sidecar (default; fails if already running)"
            echo "  stop     Stop running sidecar"
            echo "  restart  Stop then start"
            echo ""
            echo "Positional args (equivalent to options):"
            echo "  profile  Build profile: production|local (default: production)"
            echo "  port     Listen port (default: 23767)"
            echo ""
            echo "Options:"
            echo "  --port PORT       Listen port (default: 23767)"
            echo "  --profile PROF    Build profile: production|local (default: production)"
            echo "  --keep-logs       Keep previous log instead of archiving"
            echo ""
            echo "Environment:"
            echo "  AF_SIDECAR_LOG_DIR   SideCar log directory (default: logs/)"
            echo "  AF_SIDECAR_CONFIG    SideCar 配置文件路径（默认 configs/\$AGENTFLOW_ENV/sidecar.json）"
            echo "  AGENTFLOW_ENV        dev|test|prod，选择 sidecar.json 环境（默认按 profile 对齐）"
            exit 0 ;;
        *)
            # 位置参数：第一个为 profile，第二个为 port
            if [ -z "$PROFILE" ]; then
                PROFILE="$1"
            elif [ -z "$PORT" ]; then
                PORT="$1"
            else
                echo "[ERROR] Unknown argument: $1" >&2
                exit 2
            fi
            shift ;;
    esac
done

PROFILE="${PROFILE:-production}"
PORT="${PORT:-23767}"

# ── PROFILE → AGENTFLOW_ENV 对齐（sidecar.json 按环境选择）──
# sidecar 配置查找默认走 configs/{AGENTFLOW_ENV}/sidecar.json；若调用方未显式设置
# AGENTFLOW_ENV，则按 PROFILE 对齐（production→prod，local→dev），避免
# `start_sidecar.sh production` 误加载 dev 配置。start_production.sh 已 export AGENTFLOW_ENV=prod，
# 此处 `:-` 判定不会覆盖它。
if [ -z "${AGENTFLOW_ENV:-}" ]; then
    case "$PROFILE" in
        production) export AGENTFLOW_ENV="prod" ;;
        local)      export AGENTFLOW_ENV="dev" ;;
    esac
fi
# 配置文件路径：AF_SIDECAR_CONFIG 显式指定优先，否则按环境默认
SIDECAR_CONFIG="${AF_SIDECAR_CONFIG:-$AR_PROJECT_ROOT/configs/$AGENTFLOW_ENV/sidecar.json}"
if [ ! -f "$SIDECAR_CONFIG" ]; then
    echo "[WARN] SideCar config not found: $SIDECAR_CONFIG (sidecar 将回退默认 outfit-assistant)" >&2
fi

# ── 加载环境配置（必须在 PROFILE→AGENTFLOW_ENV 对齐之后）──
# ar_load_env 按 AGENTFLOW_ENV 选 configs/{env}/.env；提前执行会在
# `start_sidecar.sh production`（未显式设 AGENTFLOW_ENV）时误加载 dev 的 .env。
# .env 中的变量（如 AF_BR_COREMIND_AGENT_ID）经 set -a export，被 sidecar 子进程继承，
# 从而在启动时覆盖 sidecar.json 的 agent.name——即"只改 .env 不改代码"的路由切换入口。
ar_load_env

# ═══════════════════════════════════════════════════════════════════
#  stop
# ═══════════════════════════════════════════════════════════════════
if [ "$ACTION" = "stop" ]; then
    PID=$(_resolve_pid)
    if [ -z "$PID" ]; then
        echo "[INFO] No running sidecar found"
        exit 0
    fi
    _stop_sidecar "$PID"
    exit 0
fi

# ═══════════════════════════════════════════════════════════════════
#  restart → stop first, then fall through to start
# ═══════════════════════════════════════════════════════════════════
if [ "$ACTION" = "restart" ]; then
    PID=$(_resolve_pid)
    if [ -n "$PID" ]; then
        _stop_sidecar "$PID"
    fi
fi

# ═══════════════════════════════════════════════════════════════════
#  start
# ═══════════════════════════════════════════════════════════════════

# ── Guard: already running? ───────────────────────────────────────
# 幂等：已运行视为成功（exit 0），对齐 init.sh 注释"start_sidecar.sh 已改为 already running 返回 0"。
# 背景（2026-08-18 修复）：start_production.sh 用 `wait $! || exit 1` 校验 wrapper 退出码——
# 业务要求 sidecar 先行（init.sh 阶段 b 已启动），start_production.sh 再启时若这里 exit 1，
# 一键启动（start.sh）会误报 "[ERROR] SideCar wrapper exited with code 1" 并中断整链。
# 需要强制重启请显式用 `$0 restart`；stop 语义不变。
PID=$(_resolve_pid)
if [ -n "$PID" ]; then
    echo "[WARN] SideCar is already running (PID=$PID), start is a no-op (idempotent)"
    echo "       To restart, use:  $0 restart"
    echo "       To stop, use:     $0 stop"
    exit 0
fi

# ── Resolve binary ──────────────────────────────────────────────────
# 必须 export：sidecar_router.cpp（生产 CoreMind 集成路径）用 get_env_or("BRIDGE_URL", ...)
# 读环境变量，不是命令行参数。不 export 时 sidecar_router 用代码默认值 http://127.0.0.1:8199
# （不含 /health），导致转发 path 缺 /health 前缀，bridge 新架构（app.mount("/health", bridge_app)）404。
# main.cpp 的 configure_server 路径则通过 --bridge-url 命令行参数读取（L319 nohup 行已传），
# 两条路径都需要覆盖，所以既要 export 又要 --bridge-url。
export BRIDGE_URL="${PYTHON_BRIDGE_URL:-http://127.0.0.1:8199/health}"
case "$PROFILE" in
    production) BINARY="$AR_PROJECT_ROOT/sidecar/build-production/coremind_bridge" ;;
    local)      BINARY="$AR_PROJECT_ROOT/sidecar/build-dev/coremind_bridge" ;;
    *) echo "[ERROR] Profile must be production or local" >&2; exit 2 ;;
esac

if [ "$PROFILE" = "local" ]; then
    echo "[sidecar] Building for local profile..."
    BUILD_DIR="$AR_PROJECT_ROOT/sidecar/build-dev"
    if [ ! -d "$BUILD_DIR" ]; then
        cmake -S "$AR_PROJECT_ROOT/sidecar" -B "$BUILD_DIR"
    fi
    cmake --build "$BUILD_DIR" --target coremind_bridge -j4
    if [ ! -x "$BINARY" ]; then
        echo "[ERROR] Build failed: $BINARY not produced" >&2
        exit 1
    fi
    echo "[sidecar] Build succeeded"
elif [ ! -x "$BINARY" ]; then
    echo "[ERROR] Missing SideCar binary: $BINARY" >&2
    exit 1
fi

# ── CoreMind 运行时环境变量 ─────────────────────────────────────────
# CPU 架构检测（与工作版 start.sh 对齐）
CPU_ARCH=$(arch)
export CPU_ARCHITECTURE="${CPU_ARCHITECTURE:-X86}"
if [ "$CPU_ARCH" == "aarch64" ]; then
    export CPU_ARCHITECTURE="${CPU_ARCHITECTURE:-ARM}"
fi

export MALLOC_ARENA_MAX="${MALLOC_ARENA_MAX:-1}"
export START_UDS_SERVER="${START_UDS_SERVER:-on}"

export APPID="${APPID:-0}"
export COREMIND_REQUIRED_SERVICES="${COREMIND_REQUIRED_SERVICES:-}"
export COREMIND_OPTIONAL_SERVICES="${COREMIND_OPTIONAL_SERVICES:-}"
export SERVICENAME="${SERVICENAME:-OutfitAgent}"
export PROCESS_TYPE="${PROCESS_TYPE:-OutfitAgent}"
export CSP_PROCESS_ID="${CSP_PROCESS_ID:-OutfitAgent}"
export SYSTEM_AGENT_NAME="${SYSTEM_AGENT_NAME:-OutfitAgent}"
export TZ="${TZ:-Asia/Shanghai}"
export RUNLOG_CONFIG="${RUNLOG_CONFIG:-/opt/coremind/conf/log.json}"
export LOG_CONFIG="${LOG_CONFIG:-/opt/coremind/conf/log.json}"
export NON_BYPASS_BBOX_PATH="${NON_BYPASS_BBOX_PATH:-/opt/coremind/logs/bbox}"
export SSLPATH="${SSLPATH:-/opt/sharevolume/ssl/cert}"
export SSL_CERT_PATH="${SSL_CERT_PATH:-/opt/sharevolume/ssl/cert}"
export CIPHER_ROOT="${CIPHER_ROOT:-/opt/sharevolume/ssl/cipher}"
export CIPHER_PATH="${CIPHER_PATH:-/opt/sharevolume/ssl/cipher}"
export CIPHERPATH="${CIPHERPATH:-/opt/sharevolume/ssl/cipher}"
export ROOT_KEY_HOME="${ROOT_KEY_HOME:-/opt/sharevolume/ssl/cipher}"
export WORK_KEY_HOME="${WORK_KEY_HOME:-/opt/sharevolume/ssl/cipher}"
export INNER_TLS_ENABLE="${INNER_TLS_ENABLE:-false}"
export SSL_ENABLE="${SSL_ENABLE:-false}"

# GaussDB (DataMindSDK_C) SSL 配置路径 — 与 OutfitAgent start.sh 对齐
export RAG_GAUSS_SSL_PATH="${RAG_GAUSS_SSL_PATH:-/opt/sharevolume/ragssl}"
export RAG_GAUSS_GUC_PATH="${RAG_GAUSS_GUC_PATH:-/opt/sharevolume/ragssl}"

# ── Local profile: redirect /opt/* paths to project-local dirs ──────
# Stub mode (COREMIND_ENABLED=OFF) doesn't need real CoreMind paths.
if [ "$PROFILE" = "local" ]; then
    _LOCAL_RUNTIME="$AR_PROJECT_ROOT/.runtime/sidecar"
    export SSLPATH="${_LOCAL_RUNTIME}/ssl/cert"
    export SSL_CERT_PATH="${_LOCAL_RUNTIME}/ssl/cert"
    export RUNLOG_CONFIG="${RUNLOG_CONFIG:-$_LOCAL_RUNTIME/conf/log.json}"
    export LOG_CONFIG="${LOG_CONFIG:-$_LOCAL_RUNTIME/conf/log.json}"
    export NON_BYPASS_BBOX_PATH="${_LOCAL_RUNTIME}/logs/bbox"
    export CIPHER_ROOT="${_LOCAL_RUNTIME}/ssl/cipher"
    export CIPHER_PATH="${_LOCAL_RUNTIME}/ssl/cipher"
    export CIPHERPATH="${_LOCAL_RUNTIME}/ssl/cipher"
    export ROOT_KEY_HOME="${_LOCAL_RUNTIME}/ssl/cipher"
    export WORK_KEY_HOME="${_LOCAL_RUNTIME}/ssl/cipher"
fi

# 打印 CoreMind 框架关键 env vars
echo "[sidecar] CoreMind init env vars:"
echo "  SSLPATH=$SSLPATH"
echo "  SSL_CERT_PATH=$SSL_CERT_PATH"
echo "  SSL_ENABLE=$SSL_ENABLE"
echo "  INNER_TLS_ENABLE=$INNER_TLS_ENABLE"
echo "  SERVICENAME=$SERVICENAME"
echo "  PROCESS_TYPE=$PROCESS_TYPE"
echo "  APPID=$APPID"
echo "  LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}"
echo "  TZ=${TZ:-}"
echo "  RUNLOG_CONFIG=${RUNLOG_CONFIG:-}"
echo "  NON_BYPASS_BBOX_PATH=${NON_BYPASS_BBOX_PATH:-}"
echo "  CIPHER_PATH=${CIPHER_PATH:-}"
echo "  ROOT_KEY_HOME=${ROOT_KEY_HOME:-}"
if [ -d "$SSLPATH" ]; then
    echo "  [ok] SSLPATH directory exists"
    ls -la "$SSLPATH/" | head -10
else
    echo "  [WARN] SSLPATH directory MISSING"
fi

# ── 运行时库路径 ────────────────────────────────────────────────────
LIBRARY_PATH=""
_LIB_DIRS=(
    "$AR_PROJECT_ROOT/build/production"
    "$AR_PROJECT_ROOT/../platform/lib"
)
for _dir in "${_LIB_DIRS[@]}"; do
    if [ -d "$_dir" ]; then
        _RESOLVED_DIR=$(cd "$_dir" 2>/dev/null && pwd)
        if [ -n "$_RESOLVED_DIR" ]; then
            LIBRARY_PATH="${LIBRARY_PATH:+$LIBRARY_PATH:}$_RESOLVED_DIR"
        fi
    fi
done
export LD_LIBRARY_PATH="${AF_LIBRARY_PATH:-${LIBRARY_PATH:-}}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# CoreMind 框架 initServiceConfig 要求 SSLPATH 目录存在
mkdir -p "$SSLPATH"

# ── GaussDB SSL 配置工具部署（幂等，对齐 HealthAgent/OutfitAgent install.sh）──
# DataMindSDK_C 通过 RAG_GAUSS_GUC_PATH 定位 gs_guc 完成 SSL 配置（PGSSLCERT）。
# 检查顺序：部署目标就绪优先（install.sh/initContainer 已部署则直接复用，不重复部署、
# 不误报）→ 目标缺失时从候选源部署（gs_guc 实际由构建链注入到 $COREMIND_ROOT/gs_guc，
# 而非 $AR_PROJECT_ROOT/gs_guc——2026-08-18 修复：源路径候选化，消除路径假设不一致误报）。
# 幂等性：mkdir -p + cp -f + chmod 均可重复执行；不使用 rm -rf，避免误删目录内
# 其他文件（如证书），支持统一脚本与本脚本混用同一 ragssl 目录。
# local (stub) 模式通常无 gs_guc，缺失时告警不阻断启动。
RAG_GAUSS_GUC_BIN="$RAG_GAUSS_GUC_PATH/gs_guc"
if [ -f "$RAG_GAUSS_GUC_BIN" ]; then
    echo "[sidecar] GaussDB gs_guc already ready at $RAG_GAUSS_GUC_BIN (skip deploy)"
else
    GS_GUC_SRC=""
    for _cand in "${COREMIND_ROOT:-/opt/coremind}/gs_guc" "$AR_PROJECT_ROOT/gs_guc"; do
        if [ -f "$_cand" ]; then
            GS_GUC_SRC="$_cand"
            break
        fi
    done
    if [ -n "$GS_GUC_SRC" ]; then
        mkdir -p "$RAG_GAUSS_GUC_PATH"
        cp -f "$GS_GUC_SRC" "$RAG_GAUSS_GUC_BIN"
        chmod 550 "$RAG_GAUSS_GUC_BIN"
        echo "[sidecar] GaussDB gs_guc deployed from $GS_GUC_SRC to $RAG_GAUSS_GUC_PATH"
    else
        echo "[sidecar][WARN] gs_guc not ready: $RAG_GAUSS_GUC_BIN missing and no source found (${COREMIND_ROOT:-/opt/coremind}/gs_guc, $AR_PROJECT_ROOT/gs_guc) — GaussDB SSL 连接将失败（连接池全 dead）"
    fi
fi

# ── 生成 serviceconfig.json 到共享卷（仅 production 模式）─────────────
# local (stub) 模式不调 init_coremind_framework，不需要 serviceconfig。
if [ "$PROFILE" = "production" ]; then
    SERVICECONFIG_DIR="/opt/sharevolume/serviceconfig"
    SERVICECONFIG_JSON="$SERVICECONFIG_DIR/serviceconfig.json"
    mkdir -p "$SERVICECONFIG_DIR"

    if [ -f "$SERVICECONFIG_JSON" ]; then
        echo "[sidecar] serviceconfig.json already exists, skipping generation"
    else
        echo "[sidecar] serviceconfig.json not found, generating..."
        SERVICECONFIG_PY=""
        for candidate in "$SCRIPT_DIR/serviceconfig.py" "/opt/coremind/scripts/serviceconfig.py"; do
            [ -f "$candidate" ] && SERVICECONFIG_PY="$candidate" && break
        done
        if [ -z "$SERVICECONFIG_PY" ]; then
            echo "[ERROR] serviceconfig.py not found and serviceconfig.json not pre-generated" >&2
            exit 1
        fi
        python "$SERVICECONFIG_PY" || {
            echo "[ERROR] serviceconfig.py failed to generate JSON" >&2
            exit 1
        }
        if [ ! -f "$SERVICECONFIG_JSON" ]; then
            echo "[ERROR] serviceconfig.json was not generated after running $SERVICECONFIG_PY" >&2
            exit 1
        fi
        echo "[sidecar] serviceconfig.json generated successfully"
    fi
fi

# ── 日志文件 ─────────────────────────────────────────────────────────
LOG_DIR="${AF_SIDECAR_LOG_DIR:-$AR_PROJECT_ROOT/logs}"
LOG_FILE="$LOG_DIR/sidecar.log"
mkdir -p "$LOG_DIR"
ar_archive_log "$LOG_FILE" "$KEEP_LOGS"

# ── SidecarDumper trace 持久化（每请求独立 JSONL 文件）──
# SidecarDumper（router_logic.cpp）按 request_id 生成 {ts}_{request_id}.jsonl，
# 记录请求体 + SSE chunks + 回调事件。默认关闭（不设环境变量时 dump 方法 no-op）。
# 文件数 = 请求数，长期运行会堆积——设 AF_SIDECAR_DUMP_KEEP_DAYS=0 可禁用，或定期清理。
export AF_SIDECAR_DUMP_DIR="${AF_SIDECAR_DUMP_DIR:-$LOG_DIR/sidecar_dumps}"
mkdir -p "$AF_SIDECAR_DUMP_DIR"

# ── 日志轮转（copytruncate 模式）──
# sidecar.log 是 C++ stderr 被 shell `>>` 重定向，没有 Python RotatingFileHandler 那样的自动轮转。
# 用后台 watcher 定期检查大小，超过阈值就 copy + truncate（不动 inode，fd 仍有效）。
# 阈值默认 50MB，可通过 AF_SIDECAR_LOG_MAX_MB 覆盖。
AF_SIDECAR_LOG_MAX_MB="${AF_SIDECAR_LOG_MAX_MB:-50}"
SIDECAR_LOG_MAX_BYTES=$(( AF_SIDECAR_LOG_MAX_MB * 1024 * 1024 ))

# ── 后台启动 ─────────────────────────────────────────────────────────
echo "[sidecar] Starting on 0.0.0.0:$PORT, logging to $LOG_FILE"
echo "[sidecar] Binary: $BINARY"
echo "[sidecar] Bridge: $BRIDGE_URL"
echo "[INFO] Starting sidecar in background..."
echo "[INFO] SideCar logs → $LOG_FILE"
echo "[INFO] View live: tail -f $LOG_FILE"

nohup "$BINARY" \
    --listen "0.0.0.0:$PORT" \
    --bridge-url "$BRIDGE_URL" \
    --config "$SIDECAR_CONFIG" \
    -processType "${SERVICENAME:-OutfitAgent}" \
    $( [ "$PROFILE" = "production" ] && echo "--coremind-mode" ) \
    >> "$LOG_FILE" 2>&1 &
SIDECAR_PID=$!
echo "$SIDECAR_PID" > "$PID_FILE"

# ── 日志轮转 watcher（copytruncate 模式）──
# 后台进程：每 60s 检查 sidecar.log 大小，超阈值就归档 + 清空。
# sidecar 退出后 watcher 自动退出（kill -0 检测）。
nohup bash -c '
    SIDECAR_PID='"$SIDECAR_PID"'
    LOG_FILE="'"$LOG_FILE"'"
    MAX_BYTES='"$SIDECAR_LOG_MAX_BYTES"'
    while kill -0 "$SIDECAR_PID" 2>/dev/null; do
        sleep 60
        if [ -f "$LOG_FILE" ]; then
            size=$(stat -c%s "$LOG_FILE" 2>/dev/null || stat -f%z "$LOG_FILE" 2>/dev/null || echo 0)
            if [ "$size" -gt "$MAX_BYTES" ]; then
                cp "$LOG_FILE" "${LOG_FILE}.$(date +%Y%m%d%H%M%S)"
                : > "$LOG_FILE"
            fi
        fi
    done
' >> /dev/null 2>&1 &
LOG_WATCHER_PID=$!
echo "[INFO] Log rotator started (PID=$LOG_WATCHER_PID, max=${AF_SIDECAR_LOG_MAX_MB}MB)"

echo "[INFO] SideCar started (PID=$SIDECAR_PID)"
echo "[INFO] Dump dir: $AF_SIDECAR_DUMP_DIR"
echo "[INFO] Stop: $0 stop"
