#!/bin/bash
python3 - <<'PY'
from datetime import datetime, timedelta
print('[skill_date] now=', datetime.utcnow().isoformat()+'Z', 'plus7d=', (datetime.utcnow()+timedelta(days=7)).date())
PY
