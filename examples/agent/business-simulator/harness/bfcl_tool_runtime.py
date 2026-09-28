"""Private BFCL function execution bridge used by generated skill tools.

Only the isolated evaluator sets ``BFCL_RUNTIME_STATE``.  The bridge reads the
row from that per-trial directory, executes the official deterministic BFCL
fixture, and appends compact call facts to a private JSONL file.  It never
prints fixture results or exception text, which keeps native traces free of
credentials and bulky fixture payloads.
"""
from __future__ import annotations

import fcntl
import json
import os
from pathlib import Path
import pickle
import re
import tempfile
import time
from typing import Any


def _load_state() -> tuple[dict, dict]:
    path = Path(os.environ.get("BFCL_RUNTIME_STATE", ""))
    if not path.is_file():
        raise RuntimeError("bfcl_runtime_state_missing")
    state = json.loads(path.read_text(encoding="utf-8"))
    row_path = Path(state.get("row_file", ""))
    if not row_path.is_file():
        raise RuntimeError("bfcl_row_runtime_missing")
    return state, json.loads(row_path.read_text(encoding="utf-8"))


def _restore_aliases(value: Any, aliases: dict[str, str]):
    inverse = {str(alias): str(original) for original, alias in aliases.items()}
    if isinstance(value, dict):
        return {inverse.get(str(key), str(key)): _restore_aliases(child, aliases)
                for key, child in value.items()}
    if isinstance(value, list):
        return [_restore_aliases(child, aliases) for child in value]
    return value


def _call_text(name: str, args: dict) -> str:
    # repr is consumed by the official BFCL executor; model text is represented
    # as data and is never evaluated as Python source by this module.
    pieces = [f"{key}={value!r}" for key, value in args.items()]
    return f"{name}({', '.join(pieces)})"


def _compact_result(value: Any, limit: int = 4096) -> str:
    """Keep the private observation small while preserving the next-turn fact."""
    if isinstance(value, str):
        text = value
    else:
        text = json.dumps(value, ensure_ascii=False, default=str, separators=(",", ":"))
    return text[:limit]


def _append_log(path: Path, item: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as stream:
        fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
        stream.write(json.dumps(item, ensure_ascii=False, separators=(",", ":")) + "\n")
        stream.flush()
        fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def _visible_result(value: Any) -> Any:
    """Keep deterministic fixture errors from turning into AgentFlow failures.

    A BFCL function may legitimately return an ``error`` field (for example,
    an unsupported directory transition). That is a benchmark observation,
    not a failed skill transport. Wrap it in a completed envelope for the
    skill model while retaining the original value in the private journal.
    """
    parsed = value
    if isinstance(value, str):
        try:
            parsed = json.loads(value)
        except (TypeError, ValueError):
            return value
    if isinstance(parsed, dict) and (
        parsed.get("error") or parsed.get("ok") is False or parsed.get("success") is False
    ):
        return json.dumps({"status": "completed", "bfcl_result": parsed},
                          ensure_ascii=False, separators=(",", ":"))
    return value


def _execute_persistent(state: dict, row: dict, call: str, record: dict,
                        started_ns: int) -> Any:
    """Keep official fixture objects alive across skill worker processes.

    The official helper caches instances only in module globals. AgentFlow
    starts another skill worker on later turns, so those globals alone would
    silently reset the world to ``initial_config``. This private snapshot is
    created by us, never supplied by the model, and contains no answer key.
    """
    from bfcl_eval.eval_checker.multi_turn_eval import multi_turn_utils

    root = Path(state["row_file"]).parent
    snapshot = root / "bfcl-fixture-state.pkl"
    lock_path = root / "bfcl-fixture-state.lock"
    model_name = str(state.get("live_model_name", "bfcl-agentflow"))
    instance_names = {
        class_name: re.sub(r"[-./]", "_", f"{model_name}_eval_{row['id']}_{class_name}_instance")
        for class_name in row["involved_classes"]
    }
    with lock_path.open("a+b") as lock:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
        instances = {}
        if snapshot.exists():
            try:
                with snapshot.open("rb") as stream:
                    instances = pickle.load(stream)
            except (EOFError, OSError, pickle.PickleError, AttributeError,
                    ImportError, ValueError):
                # A killed worker may leave a partial snapshot. Restart this
                # row's deterministic fixture from its initial configuration;
                # never let one damaged cache take down the whole experiment.
                instances = {}
        # Loading the authoritative snapshot also prevents stale module globals
        # from another invocation from winning over this trial's private state.
        for class_name, instance_name in instance_names.items():
            if class_name in instances:
                setattr(multi_turn_utils, instance_name, instances[class_name])
            else:
                multi_turn_utils.__dict__.pop(instance_name, None)
        # Skill workers may ask for the same call again while composing a
        # summary. Preserve the first deterministic observation and keep the
        # duplicate out of the BFCL call sequence.
        existing_result = None
        call_log = Path(state["call_log"])
        if call_log.is_file():
            for line in call_log.read_text(encoding="utf-8").splitlines():
                try:
                    prior = json.loads(line)
                except (TypeError, ValueError):
                    continue
                if (prior.get("status") == "completed"
                        and int(prior.get("turn", -1)) == int(state.get("turn", 0))
                        and str(prior.get("call", "")) == call):
                    existing_result = prior.get("result", "")
                    break
        if existing_result is not None:
            return _visible_result(existing_result)
        results, instances = multi_turn_utils.execute_multi_turn_func_call(
            [call], row["initial_config"], row["involved_classes"],
            model_name, str(row["id"]),
            long_context="long_context" in str(row.get("test_category", "")),
            is_evaL_run=True,
        )
        fd, temporary = tempfile.mkstemp(prefix=".bfcl-fixture-", dir=str(root))
        try:
            with os.fdopen(fd, "wb") as stream:
                pickle.dump(instances, stream, protocol=pickle.HIGHEST_PROTOCOL)
                stream.flush()
            os.replace(temporary, snapshot)
        finally:
            Path(temporary).unlink(missing_ok=True)
        value = results[0] if results else ""
        record.update(status="completed", elapsed_ms=(time.monotonic_ns() - started_ns) / 1e6,
                      result=_compact_result(value))
        # Keep journal order identical to mutation order when tools run in
        # parallel. The host uses this journal to replay the official checker.
        _append_log(Path(state["call_log"]), record)
        return value


def execute_bfcl(function_name: str, args: dict, aliases: dict[str, str]) -> str:
    """Execute one model-selected function in the current isolated row."""
    state, row = _load_state()
    restored = _restore_aliases(args if isinstance(args, dict) else {}, aliases)
    call = _call_text(function_name, restored)
    started_ns = time.monotonic_ns()
    record = {
        "turn": int(state.get("turn", 0)),
        "row_id": str(state.get("row_id", "")),
        "name": str(function_name),
        "call": call,
    }
    try:
        baseline = os.environ.get("BFCL_BASELINE_PATH") or os.environ.get(
            "BFCL_BASELINE_ROOT",
            "/root/lai-code/baselines/gorilla/berkeley-function-call-leaderboard",
        )
        if baseline and baseline not in os.sys.path:
            os.sys.path.insert(0, baseline)
        value = _execute_persistent(state, row, call, record, started_ns)
        return _visible_result(value) if isinstance(value, str) else _visible_result(
            json.dumps(value, ensure_ascii=False, default=str)
        )
    except Exception:
        # Keep one malformed call from killing the daemon.  Stable error text
        # avoids leaking fixture values and lets the strict checker reject it.
        record.update(status="failed", elapsed_ms=(time.monotonic_ns() - started_ns) / 1e6,
                      result='{"status":"failed","error":"function_execution_failed"}')
        try:
            _append_log(Path(state["call_log"]), record)
        except Exception:
            pass
        return json.dumps({"status": "failed", "error": "function_execution_failed"},
                          ensure_ascii=False, separators=(",", ":"))


__all__ = ["execute_bfcl"]
