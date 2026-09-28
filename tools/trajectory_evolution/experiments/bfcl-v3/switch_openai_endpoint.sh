#!/bin/sh
# Private operator helper. Credentials are supplied through the environment;
# never place them in this file or in experiment artifacts.
set -eu
sleep "${SWITCH_DELAY_SECONDS:-1200}"
python3 - <<'PY'
import os
from pathlib import Path

path = Path(os.environ["SWITCH_ENV_FILE"])
updates = {
    "AF_LLM_BASE_URL": os.environ["SWITCH_BASE_URL"],
    "AF_LLM_API_KEY": os.environ["SWITCH_API_KEY"],
    "OPENAI_BASE_URL": os.environ["SWITCH_BASE_URL"],
    "OPENAI_API_KEY": os.environ["SWITCH_API_KEY"],
}
# Changing credentials does not select a different baseline model.
if os.environ.get("SWITCH_MODEL"):
    updates.update(AF_LLM_MODEL=os.environ["SWITCH_MODEL"],
                   OPENAI_MODEL=os.environ["SWITCH_MODEL"])
lines = path.read_text(encoding="utf-8").splitlines()
seen = set()
result = []
for line in lines:
    key = line.split("=", 1)[0].strip() if "=" in line else ""
    if key in updates:
        result.append(f"{key}={updates[key]}")
        seen.add(key)
    else:
        result.append(line)
for key, value in updates.items():
    if key not in seen:
        result.append(f"{key}={value}")
path.write_text("\n".join(result) + "\n", encoding="utf-8")
path.chmod(0o600)
print("switched endpoint/key", path, "model preserved" if "AF_LLM_MODEL" not in updates else "model explicitly selected")
PY
