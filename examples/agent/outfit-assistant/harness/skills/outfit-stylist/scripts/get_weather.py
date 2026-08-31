"""Trusted, credential-free Open-Meteo tool for outfit-stylist."""

from __future__ import annotations

import json
import os
import subprocess
import sys

from agentflow_runtime.security import redact_secrets, safe_subprocess_environment


TOOL_NAME = "get_weather"
TOOL_DESCRIPTION = (
    "Query real current weather and a five-day forecast from Open-Meteo. "
    "The tool accepts only a city name and never receives credentials."
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "city": {
            "type": "string",
            "description": "City name, for example 北京, 上海, or Tokyo.",
        },
    },
    "required": ["city"],
    "additionalProperties": False,
}


def execute(args, sandbox_endpoint=None):
    city = str((args or {}).get("city", "")).strip()
    if not city:
        return {"status": "invalid_request", "error": "city is required"}
    if len(city) > 200 or "\x00" in city:
        return {"status": "invalid_request", "error": "city is invalid"}

    script_path = os.path.join(os.path.dirname(os.path.dirname(__file__)), "weather.py")
    try:
        completed = subprocess.run(
            [sys.executable, script_path, city],
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
            env=safe_subprocess_environment(),
        )
    except (OSError, subprocess.SubprocessError) as exc:
        safe_error, _redaction = redact_secrets(str(exc))
        return {"status": "weather_error", "error": safe_error}

    try:
        payload = json.loads(completed.stdout)
    except (TypeError, json.JSONDecodeError):
        safe_error, _redaction = redact_secrets(completed.stderr.strip())
        return {
            "status": "weather_error",
            "error": safe_error or "weather provider returned invalid data",
        }
    safe_payload, _redaction = redact_secrets(payload)
    if isinstance(safe_payload, dict) and safe_payload.get("error"):
        return {"status": "weather_error", **safe_payload}
    return {"status": "success", "weather": safe_payload}
