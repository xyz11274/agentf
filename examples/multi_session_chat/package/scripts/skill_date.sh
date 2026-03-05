#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
q="${AF_USER_TEXT:-}"
if [ -z "$q" ]; then
  python3 - <<'PY'
from datetime import datetime, timedelta
now = datetime.utcnow()
print(f"[skill_date] utc_now={now.isoformat()}Z plus7d={(now+timedelta(days=7)).date()}")
PY
  exit 0
fi
parsed="$(printf '%s' "$q" | python3 "${ROOT}/scripts/date_nlp_parser.py" 2>/dev/null || true)"
answer="$(printf '%s' "$parsed" | sed -n 's/.*\"answer\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p' | head -n1)"
if [ -n "$answer" ]; then
  echo "$answer"
else
  python3 - <<'PY'
from datetime import datetime, timedelta
now = datetime.utcnow()
print(f"[skill_date] utc_now={now.isoformat()}Z plus7d={(now+timedelta(days=7)).date()}")
PY
fi
