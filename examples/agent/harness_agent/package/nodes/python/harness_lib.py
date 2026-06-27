#!/usr/bin/env python3
"""Harness core library for the harness_agent example.

This module is the heart of the harness: a *side-effect-free* decision core
(`decide_next`) plus small helpers for catalog loading, prompt assembly,
observation truncation and skill-result envelopes.

Design notes (see docs/enhancements/038-*.md):
  * The agent loop is owned by the AgentFlow framework: the coordinator emits
    ``next_skills`` to continue, or an empty list + ``response`` to terminate.
  * ``decide_next`` is a pure function over (state, results, config) so it can be
    unit-tested directly without the framework. Node wrappers only do DataBus IO.
  * Guards (max_iterations, stuck detection, observation truncation) live here so
    the loop always converges, mirroring industry practice (OpenAI ``max_turns``,
    LangGraph ``recursion_limit``, OpenHands ``StuckDetector``,
    SWE-agent ``max_observation_length``).
"""
import hashlib
import json
import os
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# --------------------------------------------------------------------------- #
# Paths / config loading
# --------------------------------------------------------------------------- #

NODE_DIR = Path(__file__).resolve().parent
PACKAGE_DIR = NODE_DIR.parent.parent  # nodes/python -> nodes -> package
CONFIG_DIR = PACKAGE_DIR / "config"
PROMPTS_DIR = PACKAGE_DIR / "prompts"

DEFAULT_HARNESS_CONFIG: Dict[str, Any] = {
    "max_iterations": 6,
    "max_observation_length": 600,
    "stuck_repeat_threshold": 2,
    "skill_order": ["retrieve", "analyze", "compose", "finalize"],
}


def _load_json_file(path: Path, default: Any) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return default


def load_harness_config(config_dir: Optional[Path] = None) -> Dict[str, Any]:
    """Load harness_config.json merged over defaults (so partial files work)."""
    config_dir = config_dir or CONFIG_DIR
    raw = _load_json_file(config_dir / "harness_config.json", {})
    merged = dict(DEFAULT_HARNESS_CONFIG)
    if isinstance(raw, dict):
        merged.update({k: v for k, v in raw.items() if v is not None})
    return merged


def load_skills_catalog(config_dir: Optional[Path] = None) -> List[Dict[str, Any]]:
    """Load the configurable skills candidate list."""
    config_dir = config_dir or CONFIG_DIR
    data = _load_json_file(config_dir / "skills_catalog.json", {})
    if isinstance(data, dict):
        skills = data.get("skills", [])
    elif isinstance(data, list):
        skills = data
    else:
        skills = []
    return [s for s in skills if isinstance(s, dict) and s.get("name")]


def catalog_by_name(catalog: List[Dict[str, Any]]) -> Dict[str, Dict[str, Any]]:
    return {s["name"]: s for s in catalog}


# --------------------------------------------------------------------------- #
# Prompt assembly (customizable context filling before model access)
# --------------------------------------------------------------------------- #

def load_prompt_template(name: str, prompts_dir: Optional[Path] = None) -> str:
    prompts_dir = prompts_dir or PROMPTS_DIR
    path = prompts_dir / name
    try:
        return path.read_text(encoding="utf-8")
    except OSError:
        return ""


def fill_prompt(template: str, variables: Dict[str, str]) -> str:
    """Fill a ``{{var}}`` style template. Unknown placeholders become empty."""
    out = template
    for key, value in variables.items():
        out = out.replace("{{" + key + "}}", str(value))
    # Drop any leftover placeholders.
    import re

    out = re.sub(r"{{\s*[\w.]+\s*}}", "", out)
    return out


# --------------------------------------------------------------------------- #
# Skill-result envelope helpers
# --------------------------------------------------------------------------- #

def make_envelope(skill: str, status: str = "ok", result: str = "",
                  can_handle: bool = True, error: Optional[str] = None,
                  confidence: float = 1.0) -> Dict[str, Any]:
    """Structured skill-result envelope used across the loop."""
    return {
        "skill": skill,
        "status": status,            # ok | unsupported | error
        "can_handle": bool(can_handle),
        "result": result,
        "error": error,
        "confidence": float(confidence),
    }


def truncate_observation(envelope: Dict[str, Any], max_len: int) -> Dict[str, Any]:
    """Truncate the ``result`` field only, preserving the JSON envelope.

    This keeps the outer structure parseable (mirrors SWE-agent
    ``max_observation_length`` while avoiding mid-JSON truncation).
    """
    out = dict(envelope)
    result = out.get("result", "")
    if isinstance(result, str) and max_len >= 0 and len(result) > max_len:
        out["result"] = result[:max_len] + "...[truncated]"
        out["truncated"] = True
    return out


def results_signature(results: Dict[str, Dict[str, Any]]) -> str:
    """Stable hash of accumulated results, for stuck detection."""
    canonical = json.dumps(results, sort_keys=True, ensure_ascii=False)
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()[:16]


# --------------------------------------------------------------------------- #
# Loop state
# --------------------------------------------------------------------------- #

def new_state() -> Dict[str, Any]:
    return {
        "iteration": 0,
        "executed_skills": [],
        "last_skill": "",
        "last_observation_hash": "",
        "stuck_count": 0,
        "terminated_reason": "",
    }


def _expected_parameters_ok(skill_meta: Dict[str, Any],
                            available: Dict[str, str]) -> Tuple[bool, List[str]]:
    expected = skill_meta.get("expected_parameters") or []
    if isinstance(expected, dict):
        expected = list(expected.keys())
    missing = [p for p in expected if p not in available or available.get(p) in (None, "")]
    return (len(missing) == 0, missing)


# --------------------------------------------------------------------------- #
# decide_next — the pure decision core
# --------------------------------------------------------------------------- #

def decide_next(state: Dict[str, Any],
                catalog: List[Dict[str, Any]],
                results: Dict[str, Dict[str, Any]],
                config: Dict[str, Any],
                available_parameters: Optional[Dict[str, str]] = None
                ) -> Dict[str, Any]:
    """Decide the next harness step. Pure: no IO, no side effects.

    Returns a dict with:
      * ``next_skills``: list of skill names to launch next (empty => terminate)
      * ``response``: final response text (only when terminating)
      * ``terminated_reason``: "" | completed | max_iterations | stuck | no_candidate
      * ``state``: updated loop state (caller persists it)

    The result-aware state machine advances strictly based on which skill
    results have accumulated, so the loop is genuinely result-driven rather
    than a static DAG.
    """
    available_parameters = available_parameters or {}
    state = dict(state)
    meta = catalog_by_name(catalog)
    order: List[str] = config.get("skill_order", DEFAULT_HARNESS_CONFIG["skill_order"])
    max_iterations = int(config.get("max_iterations", 6))
    stuck_threshold = int(config.get("stuck_repeat_threshold", 2))
    max_obs = int(config.get("max_observation_length", 600))

    state["iteration"] = int(state.get("iteration", 0)) + 1

    def terminate(reason: str, response: str) -> Dict[str, Any]:
        state["terminated_reason"] = reason
        return {"next_skills": [], "response": response,
                "terminated_reason": reason, "state": state}

    # --- Guard: stuck detection (no new results since last decision) -------- #
    sig = results_signature(results)
    if sig == state.get("last_observation_hash", "") and state.get("iteration", 1) > 1:
        state["stuck_count"] = int(state.get("stuck_count", 0)) + 1
    else:
        state["stuck_count"] = 0
    state["last_observation_hash"] = sig

    if state["stuck_count"] >= stuck_threshold:
        return terminate("stuck", _compose_response(results, meta, max_obs,
                                                     prefix="（已收敛，无新增结果）"))

    # --- Guard: max iterations --------------------------------------------- #
    if state["iteration"] > max_iterations:
        return terminate("max_iterations",
                         _compose_response(results, meta, max_obs,
                                           prefix="（已达最大迭代，给出当前最优答复）"))

    # --- Result-aware state machine ---------------------------------------- #
    # Find the first skill in declared order whose result is not yet present.
    next_skill: Optional[str] = None
    for name in order:
        if name == "finalize":
            continue
        if name not in results:
            next_skill = name
            break

    if next_skill is None:
        # All non-final skills have results. If finalize hasn't produced a
        # final_result yet, run finalize; otherwise terminate (coordinator's job).
        if "finalize" in order and "finalize" not in results:
            next_skill = "finalize"
        else:
            final_env = results.get("finalize", {})
            response = final_env.get("result") or _compose_response(results, meta, max_obs)
            return terminate("completed", response)

    # --- Parameter-slot validation before emitting next_skills ------------- #
    skill_meta = meta.get(next_skill, {})
    ok, missing = _expected_parameters_ok(skill_meta, available_parameters)
    if not ok:
        # Skip a skill whose required parameters are unavailable: record a
        # synthetic unsupported result so the loop can advance instead of stalling.
        results = dict(results)
        results[next_skill] = make_envelope(
            next_skill, status="unsupported", can_handle=False,
            result="", error="missing_parameters:" + ",".join(missing),
            confidence=0.0)
        state.setdefault("executed_skills", []).append(next_skill)
        state["last_skill"] = next_skill
        # Re-decide with the synthetic result in place (bounded by max_iterations).
        return decide_next(state, catalog, results, config, available_parameters)

    state.setdefault("executed_skills", []).append(next_skill)
    state["last_skill"] = next_skill
    return {"next_skills": [next_skill], "response": "",
            "terminated_reason": "", "state": state}


def _compose_response(results: Dict[str, Dict[str, Any]],
                      meta: Dict[str, Dict[str, Any]],
                      max_obs: int,
                      prefix: str = "") -> str:
    """Compose a final response from accumulated skill results."""
    fragments: List[str] = []
    for skill, env in results.items():
        if skill == "finalize":
            continue
        env = truncate_observation(env, max_obs)
        if env.get("status") == "ok" and env.get("result"):
            title = meta.get(skill, {}).get("title", skill)
            fragments.append(f"[{title}] {env['result']}")
    body = "；".join(fragments) if fragments else "当前可用技能未能产出有效结果。"
    return (prefix + body).strip()


# --------------------------------------------------------------------------- #
# Optional LLM bridge (real LLM when configured, deterministic fallback else)
# --------------------------------------------------------------------------- #

def llm_configured() -> bool:
    return bool(os.environ.get("AF_LLM_API_KEY") and os.environ.get("AF_LLM_BASE_URL"))


def llm_complete(prompt: str, fallback: str) -> str:
    """Best-effort LLM call; returns ``fallback`` when not configured or on error.

    Kept dependency-free (urllib) and short-timeout so offline/CI runs are
    deterministic via the fallback path.
    """
    if not llm_configured():
        return fallback
    try:  # pragma: no cover - network path not exercised in offline tests
        import json as _json
        import urllib.request

        base = os.environ["AF_LLM_BASE_URL"].rstrip("/")
        api_key = os.environ["AF_LLM_API_KEY"]
        model = os.environ.get("AF_LLM_MODEL", "gpt-4o-mini")
        timeout = float(os.environ.get("AF_LLM_TIMEOUT_SEC", "5"))
        payload = _json.dumps({
            "model": model,
            "messages": [{"role": "user", "content": prompt}],
            "temperature": 0.2,
        }).encode("utf-8")
        req = urllib.request.Request(
            base + "/chat/completions", data=payload,
            headers={"Authorization": f"Bearer {api_key}",
                     "Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = _json.loads(resp.read().decode("utf-8"))
        return data["choices"][0]["message"]["content"].strip() or fallback
    except Exception:
        return fallback
