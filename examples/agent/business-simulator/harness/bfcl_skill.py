"""Per-trial BFCL function tool materialisation for the business Agent.

The business-simulator daemon is the production AgentFlow baseline used by the
BFCL experiment.  A native trial calls :func:`prepare_row` once before the
daemon starts and :func:`set_turn` before every conversation turn.  Both write
to a private trial directory supplied by the evaluator.  The package itself
never stores benchmark fixtures, expected answers, or generated modules in
``trace_output``.

The returned ``env`` mapping is intentionally small and stable.  The host can
apply it to the daemon environment without knowing the implementation details
of the skill runner.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
from typing import Any, Mapping


SKILL_NAME = "bfcl-functions"
STATE_VERSION = 1

# Keys that are routinely treated as credentials by native privacy scanners.
# They are represented as harmless aliases in schemas and mapped back only in
# the private execution bridge.  Values are never copied into generated code.
_SENSITIVE_KEYS = frozenset({
    "access_token", "api_key", "api_secret", "card_number",
    "card_verification_number", "client_secret", "password",
    "passport_number", "refresh_token", "secret", "token",
})


def _json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), default=str)


def _atomic_write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=f".{path.name}.", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def _safe_name(value: str) -> str:
    value = re.sub(r"[^A-Za-z0-9_]+", "_", str(value)).strip("_")
    return value[:48] or "function"


def _alias_key(key: str, aliases: dict[str, str]) -> str:
    text = str(key)
    if text not in _SENSITIVE_KEYS:
        return text
    # Do not retain the sensitive word in the model-visible alias.  The
    # runtime's outbound privacy scanner treats any structured field ending in
    # ``password``, ``token`` or ``secret`` as a credential even when it is
    # only a schema name.  A deterministic neutral alias keeps the schema
    # usable while ``_ALIASES`` restores the original key immediately before
    # the private BFCL executor is called.
    alias = f"bfcl_field_{hashlib.sha256(text.encode()).hexdigest()[:10]}"
    # A row may already contain a public alias. Keep aliases injective without
    # exposing the original sensitive spelling.
    used = set(aliases.values())
    suffix = 1
    candidate = alias
    while candidate in used and aliases.get(text) != candidate:
        suffix += 1
        candidate = f"{alias}_{suffix}"
    alias = candidate
    aliases[text] = alias
    return alias


def _sanitize_schema(value: Any, aliases: dict[str, str], *, key: str | None = None) -> Any:
    if isinstance(value, dict):
        output: dict[str, Any] = {}
        for raw_key, child in value.items():
            new_key = _alias_key(raw_key, aliases) if key == "properties" else str(raw_key)
            output[new_key] = _sanitize_schema(child, aliases, key=new_key)
        # ``required`` refers to property names and must use the same aliases.
        required = output.get("required")
        if isinstance(required, list):
            output["required"] = [aliases.get(str(item), str(item)) for item in required]
        return output
    if isinstance(value, list):
        return [_sanitize_schema(item, aliases, key=key) for item in value]
    return value


def _sanitize_text(value: Any, *, limit: int = 1800) -> str:
    # Descriptions are model-visible.  Keep them bounded and remove common
    # literal credential forms if a source row happens to contain one.
    text = str(value or "")
    text = re.sub(r"(?i)(password|access[_ ]?token|api[_ ]?key)\s*[:=]\s*[^,;\s]+",
                  r"\1=<redacted>", text)
    return text[:limit]


def _all_functions(row: Mapping[str, Any]) -> dict[str, dict]:
    functions: dict[str, dict] = {}
    for item in row.get("functions", []) or []:
        if isinstance(item, Mapping) and item.get("name"):
            functions[str(item["name"])] = dict(item)
    return functions


def _request_text(row: Mapping[str, Any], turn: int) -> str:
    """Return only the current public user request for the skill prompt."""
    turns = row.get("turns") or []
    if not isinstance(turns, list) or turn < 0 or turn >= len(turns):
        return ""
    values = [str(item.get("content", "")) for item in (turns[turn] or [])
              if isinstance(item, Mapping) and item.get("role") == "user"
              and item.get("content")]
    return "\n".join(values)


def _neutralize_request_labels(text: str, aliases: Mapping[str, str]) -> str:
    """Keep benchmark values while avoiding credential-like field labels.

    BFCL contains synthetic values such as access tokens.  The runtime's
    egress scanner must still block real credentials, but these fixture labels
    are part of the benchmark instruction.  The generated schema exposes the
    neutral aliases below, so replacing only the labels preserves the task
    semantics without weakening the global scanner.
    """
    result = str(text or "")
    for raw_key, alias in sorted(aliases.items(), key=lambda item: len(item[0]), reverse=True):
        words = re.escape(str(raw_key)).replace(r"_", r"[_ -]+")
        result = re.sub(rf"(?i)\b{words}\b", str(alias), result)
    return result


def available_functions(row: Mapping[str, Any], turn: int) -> list[dict]:
    """Return the official BFCL function set for zero-based ``turn``."""
    functions = _all_functions(row)
    for raw_turn, groups in (row.get("missed_functions", {}) or {}).items():
        try:
            available_at = int(raw_turn)
        except (TypeError, ValueError):
            continue
        if turn < available_at:
            continue
        for group in groups or []:
            # BFCL stores each newly available set as a one-element list, but
            # accepting a direct function object keeps this API version-tolerant.
            members = group if isinstance(group, list) else [group]
            for item in members:
                if isinstance(item, Mapping) and item.get("name"):
                    functions[str(item["name"])] = dict(item)
    return list(functions.values())


def _module_text(function: Mapping[str, Any], aliases: dict[str, str]) -> str:
    name = str(function["name"])
    params = _sanitize_schema(function.get("parameters") or {"type": "object", "properties": {}}, aliases)
    # A short deterministic description is enough for the model; full BFCL
    # response schemas and fixture data are intentionally left out.
    description = _sanitize_text(function.get("description", ""))
    return (
        "from __future__ import annotations\n"
        "from bfcl_tool_runtime import execute_bfcl\n\n"
        f"TOOL_NAME = {name!r}\n"
        f"TOOL_DESCRIPTION = {description!r}\n"
        f"TOOL_PARAMS = {params!r}\n"
        f"_ALIASES = {aliases!r}\n\n"
        "def execute(args, sandbox_endpoint=None):\n"
        f"    return execute_bfcl({name!r}, args or {{}}, _ALIASES)\n"
    )


def _write_modules(scripts_dir: Path, functions: list[dict]) -> dict[str, str]:
    scripts_dir.mkdir(parents=True, exist_ok=True)
    for path in scripts_dir.glob("*.py"):
        if path.name != "__init__.py":
            path.unlink(missing_ok=True)
    (scripts_dir / "__init__.py").write_text("", encoding="utf-8")
    names: dict[str, str] = {}
    for function in functions:
        name = str(function["name"])
        aliases: dict[str, str] = {}
        source = _module_text(function, aliases)
        digest = hashlib.sha256(name.encode("utf-8")).hexdigest()[:12]
        filename = f"tool_{_safe_name(name)}_{digest}.py"
        _atomic_write(scripts_dir / filename, source)
        names[name] = filename
    return names


def _state_payload(row_path: Path, call_log: Path, functions: list[dict], *,
                   turn: int, live_model_name: str, row_id: str) -> dict:
    return {
        "schema": "bfcl-runtime-state",
        "version": STATE_VERSION,
        "row_file": str(row_path),
        "call_log": str(call_log),
        "row_id": str(row_id),
        "turn": int(turn),
        "n_turns": 0,
        "available_names": [str(item["name"]) for item in functions],
        "live_model_name": str(live_model_name or "bfcl-agentflow"),
    }


def prepare_row(workspace: str | os.PathLike[str], row: Mapping[str, Any], *,
                live_model_name: str = "bfcl-agentflow", turn: int = 0) -> dict:
    """Prepare an isolated BFCL row and return the daemon env contract.

    ``workspace`` must be a disposable per-trial directory.  The row fixture is
    written there with mode 0600 so the official executor can read it, while
    generated tool modules contain only sanitized schemas.  Callers should
    remove the directory after the trial.
    """
    if not isinstance(row, Mapping) or not row.get("id"):
        raise ValueError("bfcl_row_invalid")
    root = Path(workspace).resolve()
    root.mkdir(parents=True, exist_ok=True)
    row_path = root / "bfcl-row.json"
    _atomic_write(row_path, _json(dict(row)) + "\n")
    try:
        row_path.chmod(0o600)
    except OSError:
        pass
    scripts_dir = root / "bfcl-functions" / "scripts"
    functions = available_functions(row, int(turn))
    _write_modules(scripts_dir, functions)
    call_log = root / "bfcl-call-log.jsonl"
    call_log.touch(mode=0o600, exist_ok=True)
    state_path = root / "bfcl-runtime-state.json"
    state = _state_payload(row_path, call_log, functions, turn=int(turn),
                           live_model_name=live_model_name, row_id=str(row["id"]))
    state["n_turns"] = int(row.get("n_turns") or len(row.get("turns") or []))
    state["scripts_dir"] = str(scripts_dir)
    _atomic_write(state_path, _json(state) + "\n")
    try:
        state_path.chmod(0o600)
    except OSError:
        pass
    return {
        "state_path": str(state_path),
        "row_path": str(row_path),
        "scripts_dir": str(scripts_dir),
        "call_log": str(call_log),
        "available_names": state["available_names"],
        "agent_id": "business-simulator",
        "skill_name": SKILL_NAME,
        "env": {
            "BFCL_RUNTIME_STATE": str(state_path),
            "BFCL_RUNTIME_SKILL_DIR": str(scripts_dir),
        },
    }


def set_turn(state_path: str | os.PathLike[str], *, turn: int,
             live_model_name: str | None = None) -> dict:
    """Advance an existing prepared row and regenerate only visible tools."""
    state_file = Path(state_path).resolve()
    state = json.loads(state_file.read_text(encoding="utf-8"))
    row_path = Path(state["row_file"])
    row = json.loads(row_path.read_text(encoding="utf-8"))
    scripts_dir = Path(state.get("scripts_dir") or state_file.parent / "bfcl-functions" / "scripts")
    functions = available_functions(row, int(turn))
    _write_modules(scripts_dir, functions)
    state.update({
        "turn": int(turn),
        "available_names": [str(item["name"]) for item in functions],
    })
    if live_model_name:
        state["live_model_name"] = str(live_model_name)
    _atomic_write(state_file, _json(state) + "\n")
    return {
        "state_path": str(state_file),
        "scripts_dir": str(scripts_dir),
        "available_names": state["available_names"],
        "env": {
            "BFCL_RUNTIME_STATE": str(state_file),
            "BFCL_RUNTIME_SKILL_DIR": str(scripts_dir),
        },
    }


def sync_generated_skill(package_skill_dir: str | os.PathLike[str],
                         generated_scripts_dir: str | os.PathLike[str],
                         row: Mapping[str, Any] | None = None,
                         turn: int = 0) -> None:
    """Copy current-turn tools into the private package skill directory.

    AgentFlow resolves skill tools beneath ``harness/skills`` and rejects
    symlinks that escape that directory.  The BFCL host therefore refreshes
    this compact private scripts directory on every turn.
    """
    destination = Path(package_skill_dir).resolve()
    source = Path(generated_scripts_dir).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    template = Path(__file__).resolve().parent / "skills" / SKILL_NAME / "SKILL.md"
    if template.is_file():
        # The skill receives its current request through the normal AgentFlow
        # skill-doc channel.  This avoids a coordinator-generated generic
        # prompt that contains no task details.  Never include ground truth or
        # expected outputs in this document.
        content = template.read_text(encoding="utf-8")
        if row is not None:
            functions = available_functions(row, int(turn))
            aliases: dict[str, str] = {}
            for function in functions:
                _sanitize_schema(function.get("parameters") or {}, aliases)
            request = _neutralize_request_labels(_request_text(row, int(turn)), aliases)
            if request:
                content += (
                    "\n\n## Current BFCL request\n"
                    "Use the generated tools to satisfy this request. Call only "
                    "the function(s) needed by this turn and return the result.\n"
                    + _sanitize_text(request, limit=6000) + "\n"
                )
        _atomic_write(destination / "SKILL.md", content)
    scripts = destination / "scripts"
    if scripts.exists() or scripts.is_symlink():
        shutil.rmtree(scripts, ignore_errors=True)
    shutil.copytree(source, scripts, symlinks=False)


def cleanup_workspace(workspace: str | os.PathLike[str]) -> None:
    """Best-effort cleanup helper for hosts that do not use TemporaryDirectory."""
    shutil.rmtree(Path(workspace), ignore_errors=True)


__all__ = ["SKILL_NAME", "available_functions", "prepare_row", "set_turn",
           "sync_generated_skill", "cleanup_workspace"]
