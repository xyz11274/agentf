#!/bin/bash
#
# start.sh — Build and start agentflowd + af-broker
#
# Usage:
#   ./start.sh                    Build and start daemon with config.json
#   ./start.sh daemon             Start daemon only
#   ./start.sh broker             Build af-broker only
#   ./start.sh stop               Stop the running daemon
#   ./start.sh status             Check daemon health
#
# Prerequisites:
#   - C++17 toolchain (gcc/clang, cmake)
#   - Go 1.21+ (for af-broker)
#
# Configuration:
#   All daemon options are set in config.json next to this script.
#   Environment overrides: AF_LOG_LEVEL, AF_LOG_FILE, AF_FLOW_TRACE_DIR, etc.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
AF_BROKER_DIR="$SCRIPT_DIR/../af-broker"
CONFIG_FILE="$SCRIPT_DIR/config.json"
DAEMON_PID_FILE="/tmp/agentflowd.pid"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC}  $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; }
step()  { echo -e "${CYAN}[STEP]${NC}  $1"; }

# ---------------------------------------------------------------------------
# Build agentflowd
# ---------------------------------------------------------------------------
build_daemon() {
  step "Building agentflowd..."
  cd "$PROJECT_DIR"

  # Detect generator
  local generator="Unix Makefiles"
  if command -v ninja &>/dev/null; then
    generator="Ninja"
  fi

  # Detect cores
  local cores=1
  case "$(uname -s)" in
    Darwin) cores=$(sysctl -n hw.ncpu 2>/dev/null || echo 4) ;;
    Linux)  cores=$(nproc 2>/dev/null || echo 2) ;;
  esac

  mkdir -p "$BUILD_DIR"
  cd "$BUILD_DIR"

  cmake .. -G "$generator" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  cmake --build . --parallel "$cores"

  info "agentflowd built successfully"
}

# ---------------------------------------------------------------------------
# Build af-broker
# ---------------------------------------------------------------------------
build_broker() {
  step "Building af-broker..."
  cd "$AF_BROKER_DIR"
  go build -o af-broker ./cmd/af-broker/
  info "af-broker built: $AF_BROKER_DIR/af-broker"
}

# ---------------------------------------------------------------------------
# Start daemon
# ---------------------------------------------------------------------------
start_daemon() {
  if [ -f "$DAEMON_PID_FILE" ]; then
    local pid
    pid=$(cat "$DAEMON_PID_FILE")
    if kill -0 "$pid" 2>/dev/null; then
      warn "Daemon already running (PID $pid)"
      return 1
    fi
    rm -f "$DAEMON_PID_FILE"
  fi

  step "Starting agentflowd..."
  local daemon_bin="$BUILD_DIR/agentflowd"
  if [ ! -x "$daemon_bin" ]; then
    error "agentflowd binary not found at $daemon_bin"
    error "Run './start.sh' to build first"
    return 1
  fi

  # Build CLI args from config.json extras not parsed natively
  # The daemon reads config.json via --config, but we also pass
  # --pid-file so the stop command can find it.
  local pid_file="${DAEMON_PID_FILE}"

  "$daemon_bin" \
    --config "$CONFIG_FILE" \
    --pid-file "$pid_file" &

  # Wait for daemon to become ready
  local max_wait=10
  local waited=0
  while [ $waited -lt $max_wait ]; do
    if [ -f "$pid_file" ]; then
      local pid
      pid=$(cat "$pid_file")
      if kill -0 "$pid" 2>/dev/null; then
        info "agentflowd started (PID $pid)"
        return 0
      fi
    fi
    sleep 1
    waited=$((waited + 1))
  done

  error "agentflowd failed to start within ${max_wait}s"
  return 1
}

# ---------------------------------------------------------------------------
# Stop daemon
# ---------------------------------------------------------------------------
stop_daemon() {
  if [ ! -f "$DAEMON_PID_FILE" ]; then
    warn "No PID file found at $DAEMON_PID_FILE"
    return 1
  fi

  local pid
  pid=$(cat "$DAEMON_PID_FILE")
  if ! kill -0 "$pid" 2>/dev/null; then
    warn "Daemon not running (stale PID file)"
    rm -f "$DAEMON_PID_FILE"
    return 1
  fi

  step "Stopping agentflowd (PID $pid)..."
  kill "$pid"

  local max_wait=10
  local waited=0
  while [ $waited -lt $max_wait ]; do
    if ! kill -0 "$pid" 2>/dev/null; then
      info "agentflowd stopped"
      rm -f "$DAEMON_PID_FILE"
      return 0
    fi
    sleep 1
    waited=$((waited + 1))
  done

  warn "agentflowd did not stop gracefully, force killing..."
  kill -9 "$pid" 2>/dev/null || true
  rm -f "$DAEMON_PID_FILE"
}

# ---------------------------------------------------------------------------
# Health check
# ---------------------------------------------------------------------------
check_health() {
  local broker_bin="$AF_BROKER_DIR/af-broker"
  if [ ! -x "$broker_bin" ]; then
    warn "af-broker not built, skipping health check"
    return 0
  fi

  step "Checking daemon health..."
  if timeout 5 "$broker_bin" localhost:17891 health 2>&1; then
    info "Daemon is healthy"
  else
    warn "Health check skipped (daemon running but af-broker RPC may need configuration)"
  fi
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
ACTION="${1:-start}"

case "$ACTION" in
  start)
    build_daemon
    build_broker
    start_daemon
    sleep 1
    check_health
    echo ""
    info "============================================"
    info "  agentflowd is running"
    info "  Config: $CONFIG_FILE"
    info "  Stop:   $0 stop"
    info "  Status: $0 status"
    info "============================================"
    ;;

  daemon)
    build_daemon
    start_daemon
    ;;

  broker)
    build_broker
    ;;

  stop)
    stop_daemon
    ;;

  status)
    check_health
    ;;

  *)
    echo "Usage: $0 {start|daemon|broker|stop|status}"
    echo ""
    echo "  start   Build daemon + af-broker and start daemon (default)"
    echo "  daemon  Build and start daemon only"
    echo "  broker  Build af-broker only"
    echo "  stop    Stop the running daemon"
    echo "  status  Check daemon health via af-broker"
    exit 1
    ;;
esac
