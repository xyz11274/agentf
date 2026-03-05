#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
msg="${AF_INPUT:-default message}"
for sid in precreated_session_1 precreated_session_2 precreated_session_3 precreated_session_4; do
  echo "${msg}" >> "${ROOT}/runtime/${sid}.inbox"
done
echo "broadcast done"
