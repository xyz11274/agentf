"""Single generic tool for the BFCL skill.

The native evaluator keeps the raw row in its private trial workspace.  This
tool receives only a function name and JSON arguments from the AgentFlow skill
runner, checks the current turn allow-list, and executes the official BFCL
fixture helper.  The call log is compact and is consumed by the host for
scoring; raw rows never enter the repository output directory.
"""
from __future__ import annotations

import fcntl
import hashlib
import json
import os
from pathlib import Path
import time

TOOL_NAME = "bfcl_call"
TOOL_DESCRIPTION = (
    "Execute one function from the current BFCL v3 function schema. "
    "Use the exact original function name and a JSON object of arguments."
)
TOOL_PARAMS = {
    "type": "object",
    "properties": {
        "function_name": {"type": "string", "description": "Exact BFCL function name."},
        "arguments": {"type": "object", "description": "Arguments matching that function schema."},
    },
    "required": ["function_name", "arguments"],
    "additionalProperties": False,
}


def _state() -> tuple[dict, dict]:
    state_path = Path(os.environ.get("BFCL_RUNTIME_STATE", ""))
    if not state_path.is_file():
        raise RuntimeError("bfcl_runtime_state_missing")
    state = json.loads(state_path.read_text(encoding="utf-8"))
    row_path = Path(str(state.get("row_file", "")))
    if not row_path.is_file():
        raise RuntimeError("bfcl_row_runtime_missing")
    return state, json.loads(row_path.read_text(encoding="utf-8"))


def _append(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as stream:
        fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
        stream.write(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n")
        stream.flush()
        fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def _call_text(name: str, arguments: dict) -> str:
    return f"{name}({', '.join(f'{k}={v!r}' for k, v in arguments.items())})"


def execute(args: dict) -> str:
    args = args if isinstance(args, dict) else {}
    state = {}
    name = args.get("function_name")
    arguments = args.get("arguments")
    if not isinstance(name, str) or not name or not isinstance(arguments, dict):
        return json.dumps({"status": "failed", "error": "invalid_bfcl_call"}, ensure_ascii=False)
    try:
        state, row = _state()
        allowed = set(state.get("available_names") or [])
        if name not in allowed:
            return json.dumps({"status": "failed", "error": "function_unavailable"}, ensure_ascii=False)
        call = _call_text(name, arguments)
        started = time.monotonic_ns()
        from bfcl_eval.eval_checker.multi_turn_eval.multi_turn_utils import execute_multi_turn_func_call
        result, _ = execute_multi_turn_func_call(
            [call], row["initial_config"], row["involved_classes"],
            str(state.get("live_model_name", "bfcl-agentflow")), str(row["id"]),
            long_context="long_context" in row.get("test_category", ""), is_evaL_run=True,
        )
        value = result[0] if result else ""
        compact = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False, default=str)
        record = {
            "turn": int(state.get("turn", 0)), "name": name, "call": call,
            "status": "completed", "elapsed_ms": round((time.monotonic_ns() - started) / 1e6, 3),
            "result_sha256": hashlib.sha256(compact.encode("utf-8")).hexdigest(),
            "result_chars": len(compact),
        }
        _append(Path(str(state.get("call_log", ""))), record)
        return compact[:8192]
    except Exception:
        try:
            _append(Path(str(state.get("call_log", ""))), {
                "turn": int(state.get("turn", 0)), "name": name,
                "status": "failed", "error": "function_execution_failed",
            })
        except Exception:
            pass
        return json.dumps({"status": "failed", "error": "function_execution_failed"}, ensure_ascii=False)
