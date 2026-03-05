#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "${ROOT}/runtime"
msg="${AF_INPUT:-}"
if [ -z "$msg" ] && [ -f "${ROOT}/runtime/latest_user_input.txt" ]; then
  msg="user=$(cat "${ROOT}/runtime/latest_user_input.txt")"
fi
[ -z "$msg" ] && msg="user=default message"
user_line="$(printf '%s\n' "$msg" | sed -n 's/^user=//p' | head -n1)"
[ -z "$user_line" ] && user_line="$msg"
printf '%s\n' "$user_line" > "${ROOT}/runtime/latest_user_input.txt"
for sid in precreated_session_1 precreated_session_2 precreated_session_3 precreated_session_4; do
  printf '%s\n' "$msg" >> "${ROOT}/runtime/${sid}.inbox"
done
echo "broadcast done"
