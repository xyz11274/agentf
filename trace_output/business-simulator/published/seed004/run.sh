#!/usr/bin/env bash
set -euo pipefail
AF_RELEASE_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../../.." && pwd)"
export PYTHONDONTWRITEBYTECODE=1
exec python3 "$AF_RELEASE_ROOT/tools/trajectory_evolution/experiments/business-coverage/publish_business_500.py" "$@"
