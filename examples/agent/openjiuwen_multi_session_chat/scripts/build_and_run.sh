#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
cd "$ROOT"

./build.sh build
./build/example_openjiuwen_multi_session_chat_tui examples/agent/openjiuwen_multi_session_chat/package
