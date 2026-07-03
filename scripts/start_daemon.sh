#!/bin/bash
# Start agentflowd. Build is explicit; pass --build when desired.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
source "$SCRIPT_DIR/common.sh"

PACKAGE="examples/agent/outfit_assistant"
PORT="17891"
PROFILE="release"
KEEP_LOGS=false
BUILD_FIRST=false

while [ "$#" -gt 0 ]; do
    case "$1" in
        --package) PACKAGE="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --profile) PROFILE="$2"; shift 2 ;;
        --keep-logs) KEEP_LOGS=true; shift ;;
        --build) BUILD_FIRST=true; shift ;;
        -h|--help)
            echo "Usage: $0 [--package PATH] [--port PORT] [--profile dev|release|production] [--build] [--keep-logs]"
            exit 0 ;;
        *)
            if [ "$PACKAGE" = "examples/agent/outfit_assistant" ]; then PACKAGE="$1"
            elif [ "$PORT" = "17891" ]; then PORT="$1"
            else echo "[ERROR] Unknown argument: $1" >&2; exit 2
            fi
            shift
            ;;
    esac
done

case "$PROFILE" in
    dev) BUILD_DIR="$AR_PROJECT_ROOT/build/dev" ;;
    release) BUILD_DIR="$AR_PROJECT_ROOT/build" ;;
    production) BUILD_DIR="$AR_PROJECT_ROOT/build/production" ;;
    *) echo "[ERROR] Invalid profile: $PROFILE" >&2; exit 2 ;;
esac

if $BUILD_FIRST; then
    "$SCRIPT_DIR/build.sh" "$PROFILE" agentflowd
fi
DAEMON="$BUILD_DIR/agentflowd"
if [ ! -x "$DAEMON" ]; then
    echo "[ERROR] Missing daemon binary: $DAEMON" >&2
    echo "        Run: ./ar_scripts/build.sh $PROFILE agentflowd" >&2
    exit 1
fi

ar_load_env
LOG_FILE="$AR_PROJECT_ROOT/logs/agentflowd.log"
ar_archive_log "$LOG_FILE" "$KEEP_LOGS"

if [[ "$PACKAGE" = /* ]]; then PACKAGE_PATH="$PACKAGE"
else PACKAGE_PATH="$AR_PROJECT_ROOT/$PACKAGE"
fi

exec "$DAEMON" \
    --package "$PACKAGE_PATH" \
    --listen "127.0.0.1:$PORT" \
    --log-file "$LOG_FILE"
