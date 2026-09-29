#!/usr/bin/env bash
set -u

# Keep the business experiment output below the user-visible 5 GiB cap.
# The guard only measures and requests a graceful stop; it never removes
# checkpoints or reports that may be needed for recovery.
ROOT_DIR="${1:?repository root is required}"
MAIN_PID="${2:?runner pid is required}"
OUTPUT_DIR="$ROOT_DIR/trace_output/business-simulator"
LOG_FILE="$OUTPUT_DIR/gepa-storage.log"
LIMIT_KIB="${STORAGE_GUARD_LIMIT_KIB:-4800000}"
INTERVAL="${STORAGE_GUARD_INTERVAL_SECONDS:-60}"

mkdir -p "$OUTPUT_DIR"
while kill -0 "$MAIN_PID" 2>/dev/null; do
  size_kib="$(du -x -s "$OUTPUT_DIR" 2>/dev/null | awk '{print $1}')"
  size_kib="${size_kib:-0}"
  now="$(date -u +%FT%TZ)"
  printf '%s size_kib=%s limit_kib=%s pid=%s\n' "$now" "$size_kib" "$LIMIT_KIB" "$MAIN_PID" >> "$LOG_FILE"
  if [ "$size_kib" -ge "$LIMIT_KIB" ]; then
    printf '%s action=SIGTERM reason=storage_limit pid=%s\n' "$now" "$MAIN_PID" >> "$LOG_FILE"
    kill -TERM "$MAIN_PID" 2>/dev/null || true
    break
  fi
  sleep "$INTERVAL"
done
printf '%s guard_exit=1\n' "$(date -u +%FT%TZ)" >> "$LOG_FILE"
