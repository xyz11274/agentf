#!/usr/bin/env python3
"""
Universal Skill Executor — parameterized skill task node.

Dispatched by coordinator via run_skills. Receives:
  - Task instruction (skill_prompt:{name}): "what to do"
  - Skill docs (load_skills:{name}): "how to do it"
  - Structured input (exec_skill_key): large/structured data

Executes LLM + skill knowledge + domain tools while-loop, outputs result.

Environment variables (injected by the worker):
  AF_INPUT               — Fallback input (coordinator's original message)
  AF_DIALOGUE_ID         — Current dialogue ID
  AF_SESSION_ID          — Current session ID
  AF_DATABUS_UDS_FILE    — UDS socket for runtime DataBus access
  AF_SKILL_NAME          — Current skill name (injected by framework, runtime.cpp)
  AF_EXEC_SKILL_KEY      — DataBus key for structured input
"""

import importlib as _importlib
import importlib.util as _importlib_util
import json
import logging
import os
import re
import sys
import time
from pathlib import Path

# ── Add nodes/ to sys.path for local imports ──
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import yaml

from agentflow_runtime.common.runtime_context import (
    dialogue_id as _dialogue_id,
    exec_skill_key as _exec_skill_key,
    input as _input,
    session_id as _session_id,
)
from agentflow_runtime.common.agent_config import AgentConfig
from agentflow_runtime.databus_client import DataBusClient
from agentflow_runtime.llm_call import (
    call_llm,
    compress_messages,
    estimate_message_tokens,
    get_compaction_threshold,
    get_model_max_tokens,
    resolve_llm_config,
)
from agentflow_runtime.observability import emit_event
from agentflow_runtime.parse_af_input import parse_stream
from prompts import (
    SKILL_ARTIFACT_AUTO_COLLECT_PROMPT,
    SKILL_FALLBACK_SYSTEM,
    SKILL_NETWORK_FAILURE_PROMPT,
    SKILL_NETWORK_RESOURCE_PROMPT,
    SKILL_NOT_FOUND_TEMPLATE,
    SKILL_RECOVERY_PROMPT,
    SKILL_SECTION_TEMPLATE,
)
from agentflow_runtime.round_reminder import build_round_reminder

NODES_DIR = os.path.dirname(os.path.abspath(__file__))
AGENT_PACKAGE_DIR = os.path.dirname(NODES_DIR)
AGENT_MD_PATH = os.path.join(AGENT_PACKAGE_DIR, "agent.md")

# Path to the built-in system tools (shipped with agentflow-runtime package)
_BUILTIN_TOOLS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "harness", "tools")


# ── §3.5 venv sentinel detection: inject .venv into PATH/PYTHONPATH ──────
# skill_executor is exec'd by shell wrapper which already selected the python
# (either .venv/bin/python3 or fallback system python3). But subprocess.run()
# inside tool execution (e.g. bash tool running "python3 script.py" from SKILL.md)
# resolves "python3" from PATH — which is system python3 by default, NOT .venv.
#
# So we detect the same sentinel the wrapper checked (§3.5), and inject .venv's
# bin/ into PATH + site-packages/ into PYTHONPATH so ALL child subprocesses inherit
# the .venv environment. User skill scripts (weather.py, outfit_vision.py, ...) thus
# run with .venv's python + dependencies WITHOUT needing to know about the framework.
#
# This is idempotent: if sentinel absent (fallback mode), we skip injection and
# child processes use system python3 (which §2.2 Bridge pre-installed agentflow-runtime into).
#
# IMPORTANT: This is a LAZY function (not module-level). At import time, the entry
# file wrapper hasn't yet set the correct AGENT_PACKAGE_DIR (which points to the
# actual agent package dir, NOT the pip package's parent). We call this from
# execute() so the entry-file override of AGENT_PACKAGE_DIR is in effect.
_venv_injected = False

def _inject_venv_env() -> None:
    global _venv_injected
    if _venv_injected:
        return  # idempotent — inject once per process
    _venv_dir = os.path.join(AGENT_PACKAGE_DIR, ".venv")
    _venv_sentinel = os.path.join(_venv_dir, ".agentflow_installed")
    # Cross-platform venv detection: Unix → bin/python3, Windows → Scripts/python.exe
    _venv_python = os.path.join(_venv_dir, "bin", "python3")
    if not os.path.isfile(_venv_python):
        _venv_python = os.path.join(_venv_dir, "Scripts", "python.exe")
    if not (os.path.isfile(_venv_python) and os.path.isfile(_venv_sentinel)):
        return  # fallback mode: system python3 (§2.2 Bridge pre-installed agentflow-runtime)
    # Inject .venv/bin to PATH front so "python3" resolves to .venv's python
    _venv_bin = os.path.join(_venv_dir, "bin")
    if not os.path.isdir(_venv_bin):
        _venv_bin = os.path.join(_venv_dir, "Scripts")
    _existing_path = os.environ.get("PATH", "")
    if _venv_bin not in _existing_path.split(os.pathsep):
        os.environ["PATH"] = _venv_bin + os.pathsep + _existing_path
    # Inject .venv's site-packages to PYTHONPATH (cross-platform: Unix vs Windows layout)
    _site_packages = []
    _lib_candidates = [
        os.path.join(_venv_dir, "lib", "python*", "site-packages"),  # Unix
        os.path.join(_venv_dir, "Lib", "site-packages"),             # Windows
    ]
    import glob as _glob
    for _pat in _lib_candidates:
        _site_packages.extend(_glob.glob(_pat))
    if _site_packages:
        _existing_pp = os.environ.get("PYTHONPATH", "")
        _new_pp = os.pathsep.join(_site_packages)
        if _new_pp not in _existing_pp:
            os.environ["PYTHONPATH"] = _new_pp + (os.pathsep + _existing_pp if _existing_pp else "")
    _venv_injected = True


# ── Helpers for entry-file-aware path resolution ──
def _get_agent_package_dir() -> str:
    return AGENT_PACKAGE_DIR

def _get_skills_base() -> str:
    return os.path.join(AGENT_PACKAGE_DIR, "harness", "skills")

def _get_builtin_tools_dir() -> str:
    """Return path to the built-in system tools directory."""
    return _BUILTIN_TOOLS_DIR

def _get_agent_tools_dir() -> str:
    """Return path to the agent package's local harness/tools (override)."""
    return os.path.join(AGENT_PACKAGE_DIR, "harness", "tools")


# ── Load .env from package root (lazy, respects entry file overrides) ──
_DOTENV_PATH = os.path.join(AGENT_PACKAGE_DIR, ".env")

def _load_dotenv() -> None:
    """Load .env using AGENT_PACKAGE_DIR from globals() (respects entry file overrides)."""
    p = os.path.join(AGENT_PACKAGE_DIR, ".env")
    print(f"[flow] dotenv: path={p} exists={os.path.isfile(p)}", file=sys.stderr)
    if os.path.isfile(p):
        try:
            from dotenv import load_dotenv
            load_dotenv(p, override=True)
            print(f"[flow] dotenv: loaded OK", file=sys.stderr)
        except Exception as exc:
            print(f"[dotenv] ERROR: load failed: {exc}", file=sys.stderr)
            logging.error("dotenv.load failed: %s", exc)

# _load_dotenv() is called from execute() to respect entry file overrides

class _LocalToolRegistry:
    """Loads domain tools from global harness/tools/ AND skill-specific scripts/.

    Global tools (harness/tools/*.py) provide atomic capabilities like bash,
    read_file, write_file, web_fetch — available to ALL skills.

    Skill-scoped tools (harness/skills/{skill_name}/scripts/*.py) provide
    domain-specific tools — only available to the owning skill.

    Each tool module must expose: TOOL_NAME, TOOL_DESCRIPTION, TOOL_PARAMS, execute(args).
    Global tools are loaded first; skill-scoped tools override same-named globals.
    """
    def __init__(self, skill_name: str = ""):
        self._tools = []
        self._modules = {}

        # 0. Inject AGENT_PACKAGE_DIR for built-in tools (bash, load_skill, etc.)
        os.environ["AF_AGENT_PACKAGE_DIR"] = AGENT_PACKAGE_DIR

        # 1. Load built-in system tools from the agentflow-runtime package
        builtin_dir = _get_builtin_tools_dir()
        if os.path.isdir(builtin_dir):
            _load_dir(self, builtin_dir)

        # 2. Load agent-specific tools from agent package (override)
        agent_dir = _get_agent_tools_dir()
        if os.path.isdir(agent_dir) and os.path.abspath(agent_dir) != os.path.abspath(builtin_dir):
            _load_dir(self, agent_dir)

        # 3. Load skill-scoped tools from harness/skills/{skill_name}/scripts/
        if skill_name:
            scripts_dir = os.path.join(_get_skills_base(), skill_name, "scripts")
            if os.path.isdir(scripts_dir):
                _load_dir(self, scripts_dir, prefix=f"_skill_{skill_name}_")

    def get_tools(self):
        return list(self._tools)

    def execute(self, name, args):
        _mod = self._modules.get(name)
        if _mod is None:
            return json.dumps({"error": f"Unknown tool: {name}"})
        try:
            return _mod.execute(args)
        except Exception as e:
            return json.dumps({"error": f"Tool '{name}' failed: {e}"})


def _load_dir(registry: _LocalToolRegistry, directory: str, prefix: str = ""):
    """Load all valid tool modules from a directory into the registry."""
    for entry in sorted(os.listdir(directory)):
        if not entry.endswith(".py") or entry.startswith("_"):
            continue
        mod_name = entry[:-3]
        try:
            spec = _importlib_util.spec_from_file_location(
                f"_tool_{prefix}{mod_name}",
                os.path.join(directory, entry))
            if spec is None or spec.loader is None:
                continue
            _mod = _importlib_util.module_from_spec(spec)
            spec.loader.exec_module(_mod)
        except Exception as exc:
            logging.warning("tool_loader.skip module=%s error=%s", name, exc)
            continue
        if not all(hasattr(_mod, a) for a in ("TOOL_NAME", "TOOL_DESCRIPTION", "TOOL_PARAMS", "execute")):
            continue
        registry._modules[_mod.TOOL_NAME] = _mod
        registry._tools.append({"type": "function", "function": {
            "name": _mod.TOOL_NAME, "description": _mod.TOOL_DESCRIPTION, "parameters": _mod.TOOL_PARAMS}})


# ---------------------------------------------------------------------------
# agent.md parsing
# ---------------------------------------------------------------------------

RE_ENV_VAR = re.compile(r"\$\{(\w+)\}")


def _expand_env(value):
    if isinstance(value, str):
        return RE_ENV_VAR.sub(lambda m: os.environ.get(m.group(1), m.group(0)), value)
    if isinstance(value, dict):
        return {k: _expand_env(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_expand_env(v) for v in value]
    return value


def parse_agent_md(path: str) -> tuple:
    if not os.path.isfile(path):
        return {}, ""
    with open(path, encoding="utf-8") as f:
        raw = f.read()
    parts = raw.split("---", 2)
    if len(parts) < 3:
        return {}, raw.strip()
    try:
        frontmatter = yaml.safe_load(parts[1]) or {}
        frontmatter = _expand_env(frontmatter)
    except Exception:
        frontmatter = {}
        print(f"[flow] WARN: agent.md frontmatter parse failed", file=sys.stderr)
    return frontmatter, parts[2].strip()


# ---------------------------------------------------------------------------
# SKILL.md loading
# ---------------------------------------------------------------------------

def load_skill_md(skill_name: str) -> str | None:
    """Read harness/skills/{skill_name}/SKILL.md and replace AgentFoundry template vars.

    AgentFoundry materializes placeholders like ${SKILL_DIR} and ${SKILLS_DIR}
    at mount time.  Our V3 coordinator doesn't have a materialization step,
    so we substitute them here with the actual filesystem paths.
    """
    path = os.path.join(_get_skills_base(), skill_name, "SKILL.md")
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as f:
        content = f.read()
    # Replace AgentFoundry template variables
    skill_dir = os.path.join(_get_skills_base(), skill_name)
    content = content.replace("${SKILL_DIR}", skill_dir)
    content = content.replace("${SKILLS_DIR}", _get_skills_base())
    return content


def load_skills_from_list(skill_names: list) -> dict:
    """Batch load SKILL.md files, returns {name: content}"""
    result = {}
    for name in skill_names:
        content = load_skill_md(name)
        result[name] = content if content else SKILL_NOT_FOUND_TEMPLATE.format(name=name)
    return result


# ---------------------------------------------------------------------------
# LLM interaction
# ---------------------------------------------------------------------------


# ---------------------------------------------------------------------------
# Runaway tool-call loop detection helpers
# ---------------------------------------------------------------------------


def _detect_tool_call_loop(
    history: list[list[tuple[str, str]]],
    new_signatures: list[tuple[str, str]],
    threshold: int = 3,
) -> bool:
    """Return True when the same tool_call signatures recur `threshold` rounds in a row.

    A "signature" is ``(tool_name, sorted_json_args)``. LLM陷入"反复调同一 tool 同一
    参数"的死循环时（实测发生过 16 轮调 weather/get_user_profile 不给最终答案），
    连续 ``threshold`` 轮签名完全一致即视为 loop，由调用方强制走完成分支。

    不变量 I-4 幂等性：同一规则重复应用不应产生重复效果——重复 tool_call 即违反。
    """
    if not new_signatures or threshold <= 0:
        return False
    # threshold=1 表示"当前轮出现即触发"——第 1 次就该返回 True，
    # 无需 history。之前漏了这行导致 threshold=1 实际要 2 轮才触发。
    if threshold == 1:
        return True
    new_key = tuple(sorted(new_signatures))
    consecutive = 1
    for past in reversed(history):
        if tuple(sorted(past)) == new_key:
            consecutive += 1
            if consecutive >= threshold:
                return True
        else:
            break
    return False


def _summarize_tool_history(messages: list) -> str:
    """Build a plain-text summary of tool interactions when LLM never gave a final answer.

    Used as the ``skill_result`` fallback when we force-complete on loop detection,
    so the user/coordinator sees *something* useful rather than an empty string
    (不变量 I-1 不丢失业务数据).

    输出必须是**纯文本**而非 Markdown——coordinator 把 ``skill_result`` 直接塞进
    ``response`` key 推给前端，前端按 Markdown 渲染。若这里输出 ``### 工具 X 的输出``
    前端会渲染成 h3 标题而非工具名，造成 UI 错位（实测发生过）。
    所以用 ``【工具 X 的输出】`` 这种方括号标签而非 ``###`` 标题语法。
    """
    chunks: list[str] = []
    for m in messages:
        role = m.get("role", "")
        if role == "tool":
            name = m.get("tool_call_id", "?")
            content = m.get("content", "")
            chunks.append(f"【工具 {name} 的输出】\n{content}")
        elif role == "assistant" and m.get("content"):
            chunks.append(f"【LLM 思考】\n{m['content']}")
    return "\n\n".join(chunks) if chunks else "(skill_executor 在工具调用循环中被强制终止，无可用输出)"


def validate_tool_calls(choice: dict) -> tuple[bool, str]:
    """Reject truncated or syntactically broken tool calls before execution."""
    if choice.get("finish_reason") == "length":
        return False, "completion_truncated"
    for tool_call in choice.get("message", {}).get("tool_calls") or []:
        function = tool_call.get("function", {})
        name = function.get("name", "unknown")
        try:
            arguments = json.loads(function.get("arguments", "{}"))
        except (json.JSONDecodeError, TypeError):
            return False, f"invalid_tool_arguments:{name}"
        if not isinstance(arguments, dict):
            return False, f"invalid_tool_arguments:{name}"
    return True, ""


def remove_invalid_tool_turns(messages: list) -> list:
    """Remove malformed assistant tool calls and their paired tool results."""
    invalid_ids = set()
    cleaned = []
    for message in messages:
        if message.get("role") == "assistant" and message.get("tool_calls"):
            choice = {"finish_reason": "tool_calls", "message": message}
            valid, _ = validate_tool_calls(choice)
            if not valid:
                invalid_ids.update(
                    call.get("id") for call in message.get("tool_calls", [])
                    if call.get("id")
                )
                continue
        if (message.get("role") == "tool" and
                message.get("tool_call_id") in invalid_ids):
            continue
        cleaned.append(message)
    return cleaned


def extract_artifact_paths(text: str) -> list[str]:
    """Extract generated image URLs/paths from a tool result.

    Handles three formats:
      - HTTP(S) URLs:         https://cdn.example/look.jpg
      - Absolute paths:       /home/user/outfit.png
      - Relative paths:       .agentfoundry/.../file.png, assets/outfit.png

    Relative paths are resolved against AGENT_PACKAGE_DIR (the cwd for tool execution).

    I-6 boundary (see artifact-output-pipeline.md §5): artifacts must be files
    the skill *generated this turn*, not inputs it consumed / referenced / read.
    We guard by excluding known user-upload input directories
    (workspace/upload/, uploads/). The framework copies generated files into
    outputs/ via _ensure_artifacts_in_package, so uploads/ never legitimately
    holds a generated artifact.
    """
    pattern = (
        r"(?:"
        r"https?://[^\s\"'\\]+"          # http(s) URLs
        r"|/[^\s\"'\\]+"                 # absolute paths
        r"|(?:\.\.?)?[^\s\"'\\:]+/[^\s\"'\\]+"  # relative paths with at least one /
        r")"
        r"\.(?:png|jpe?g|webp|gif|pptx?|docx?|xlsx?|pdf)"
    )
    candidates = re.findall(pattern, text, flags=re.IGNORECASE)
    seen: list[str] = []
    skipped_upload: list[str] = []          # I-6 boundary audit trail
    skipped_missing: list[str] = []
    for item in candidates:
        if item.startswith(("http://", "https://")):
            # External URL — no upload-dir guard applies, but still skip
            # obvious upload endpoint paths (defensive; not the main path).
            if "/upload/" in item or "/uploads/" in item:
                skipped_upload.append(item)
                continue
            seen.append(item)
        elif item.startswith("/"):
            # Absolute path — reject if it lives under an upload input directory
            if _is_user_upload_input(item):
                skipped_upload.append(item)
                continue
            if os.path.isfile(item):
                seen.append(item)
            else:
                skipped_missing.append(item)
        else:
            # Relative path — resolve against AGENT_PACKAGE_DIR
            abs_path = os.path.normpath(os.path.join(AGENT_PACKAGE_DIR, item))
            if _is_user_upload_input(abs_path):
                skipped_upload.append(abs_path)
                continue
            if os.path.isfile(abs_path):
                seen.append(abs_path)
            else:
                skipped_missing.append(abs_path)
    # I-6 audit log: 守护点证据。完整清单落 stderr，不截断（F7）。
    print(
        f"[flow] artifact.collect candidates={len(candidates)} "
        f"admitted={len(seen)} skipped_upload={len(skipped_upload)} "
        f"skipped_missing={len(skipped_missing)}",
        file=sys.stderr,
    )
    if skipped_upload:
        print(
            "[flow] artifact.skipped_upload (I-6 input boundary) "
            + json.dumps(skipped_upload, ensure_ascii=False),
            file=sys.stderr,
        )
    if skipped_missing:
        print(
            "[flow] artifact.skipped_missing (file not on disk) "
            + json.dumps(skipped_missing, ensure_ascii=False),
            file=sys.stderr,
        )
    return list(dict.fromkeys(seen))


# I-6 boundary: directories that hold user-uploaded inputs, never skill-generated artifacts.
# Match against resolved absolute paths only; keep lowercase for case-insensitive compare.
_USER_UPLOAD_INPUT_MARKERS = ("/workspace/upload/", "/uploads/")


def _is_user_upload_input(resolved_path: str) -> bool:
    """Return True if the given (resolved) path lives under a user-upload input directory.

    Used by extract_artifact_paths to enforce I-6: skill-consumed inputs must not
    be reported as generated artifacts. Case-insensitive on the directory markers
    because macOS / Linux paths are not reliably canonical.
    """
    lowered = resolved_path.lower()
    return any(marker in lowered for marker in _USER_UPLOAD_INPUT_MARKERS)


def _ensure_artifacts_in_package(artifacts: list[str]) -> list[str]:
    """Copy artifacts into package outputs/ and return updated paths.

    The Bridge artifact endpoint only serves files under agent_packages_path.
    This function copies ALL artifact files (whether inside or outside the
    package) to AGENT_PACKAGE_DIR/outputs/, giving a single predictable
    location regardless of where the LLM saved them. Avoids issues with:
      - Hidden directories (.agentfoundry/) in the URL path
      - Files saved outside the package (/tmp/...)
      - Path resolution mismatches between daemon and Bridge processes
    """
    result = []
    skipped_missing: list[str] = []
    skipped_copy_err: list[str] = []
    pkg_root = Path(AGENT_PACKAGE_DIR).resolve()
    outputs_dir = pkg_root / "outputs"
    outputs_dir.mkdir(parents=True, exist_ok=True)
    for path in artifacts:
        p = Path(path).resolve()
        if not p.is_file():
            skipped_missing.append(path)
            continue  # skip non-existent files rather than returning dead URLs
        try:
            p.relative_to(pkg_root)
        except ValueError:
            pass
        else:
            # Package-local artifacts are already publishable by Bridge. Keep
            # their directory hierarchy so HTML/CSS relative references and
            # same-named files remain stable.
            result.append(str(p))
            continue
        dest = outputs_dir / p.name
        # Deduplicate: if same filename already copied, add a suffix
        if dest.exists() and dest.read_bytes() != p.read_bytes():
            stem = dest.stem
            suffix = dest.suffix
            counter = 1
            while True:
                alt = outputs_dir / f"{stem}_{counter}{suffix}"
                if not alt.exists():
                    dest = alt
                    break
                counter += 1
        if not dest.exists():
            try:
                dest.write_bytes(p.read_bytes())
            except OSError as exc:
                skipped_copy_err.append(f"{path} -> {dest}: {exc}")
                continue  # skip files that can't be copied
        result.append(str(dest))
    # 完整审计日志（F7：不截断）。数量摘要 + 跳过清单分两行，便于 grep。
    print(
        f"[flow] artifact relocate input={len(artifacts)} relocated={len(result)} "
        f"skipped_missing={len(skipped_missing)} copy_err={len(skipped_copy_err)}",
        file=sys.stderr,
    )
    if skipped_missing:
        print(
            "[flow] artifact.relocate_missing " + json.dumps(skipped_missing, ensure_ascii=False),
            file=sys.stderr,
        )
    if skipped_copy_err:
        print(
            "[flow] artifact.relocate_copy_err " + json.dumps(skipped_copy_err, ensure_ascii=False),
            file=sys.stderr,
        )
    return result


# ---------------------------------------------------------------------------
# Main — LLM while-loop with domain tools
# ---------------------------------------------------------------------------

def execute():
    _load_dotenv()  # Load .env (respects entry file overrides of AGENT_PACKAGE_DIR)
    # §3.5 venv sentinel injection — lazy (entry-file AGENT_PACKAGE_DIR override is in effect now)
    _inject_venv_env()
    # ① Read framework-injected environment variables
    user_input     = _input()
    dialogue_id    = _dialogue_id()
    session_id     = _session_id()
    exec_skill_key = _exec_skill_key()

    databus = DataBusClient()
    # Read stream flag from DataBus (persisted by coordinator_decide from bridge input)
    stream_val = databus.get("stream") or ""
    stream = (stream_val == "true")

    # ② Read dispatch manifest from DataBus (key = exec_skill_key, e.g. "dispatch:1")
    #    Manifest: {"skill_name": "echo", "prompt": "...", "load_skills": [...]}
    skill_name = ""
    skill_prompt = ""
    load_skill_names = []

    if exec_skill_key and databus.is_connected():
        raw = databus.get_global(exec_skill_key)
        if raw:
            try:
                manifest = raw if isinstance(raw, dict) else json.loads(raw)
                skill_name = manifest.get("skill_name", "")
                skill_prompt = manifest.get("prompt", "")
                load_skill_names = manifest.get("load_skills", [])
            except (json.JSONDecodeError, TypeError):
                skill_prompt = str(raw)

    # Fallback: if no dispatch manifest, use user_input
    if not skill_name:
        skill_name = "unknown-skill"

    # Ensure own skill doc is loaded
    if skill_name and skill_name not in load_skill_names:
        load_skill_names.insert(0, skill_name)

    # ⑤ Determine final task instruction
    task_instruction = skill_prompt or user_input

    # ⑥ Load skill docs → construct system prompt
    skill_docs = load_skills_from_list(load_skill_names)
    system_parts = []
    for name, content in skill_docs.items():
        system_parts.append(SKILL_SECTION_TEMPLATE.format(name=name, content=content))
    system_prompt = "\n\n---\n\n".join(system_parts) if system_parts else \
                    SKILL_FALLBACK_SYSTEM
    # Append network-failure guard: tells LLM not to retry tools that
    # keep failing, avoiding infinite tool-call loops on network outage.
    system_prompt += "\n\n" + SKILL_NETWORK_FAILURE_PROMPT
    # Append network-resource guard: tells LLM not to retry downloading
    # resources that fail, and to return error info to the user.
    system_prompt += "\n\n" + SKILL_NETWORK_RESOURCE_PROMPT
    # Append artifact-auto-collect instruction: tells LLM that generated
    # artifacts are auto-collected and published; no need for self-hosted
    # HTTP server or file service.
    system_prompt += "\n\n" + SKILL_ARTIFACT_AUTO_COLLECT_PROMPT

    # ⑦ Construct LLM messages
    messages = [
        {"role": "system", "content": system_prompt},
        {"role": "user",   "content": task_instruction},
    ]

    # ⑧ Get domain tool set from CURRENT skill's scripts/ dir (skill-scoped)
    tool_registry = _LocalToolRegistry(skill_name)
    available_tools = tool_registry.get_tools()

    # ⑨ Read model config with fallback chain
    frontmatter, _ = parse_agent_md(AGENT_MD_PATH)
    model_cfg = frontmatter.get("model", {})
    agent_md_model = model_cfg if isinstance(model_cfg, str) else model_cfg.get("name", "")
    llm_cfg = resolve_llm_config(agent_md_model, node_type="skill")
    model_name = llm_cfg["model"]
    api_key = llm_cfg["api_key"]
    base_url = llm_cfg["base_url"]

    # ⑩ LLM while-loop — max_rounds defined before flow debug to avoid NameError
    max_rounds = AgentConfig.skill_max_iterations()

    # ── flow debug ──
    emit_event("skill_executor.trigger", {
        "exec_skill_key": exec_skill_key,
        "skill_name": skill_name,
        "load_skills": load_skill_names,
        "model": model_name,
        "max_rounds": max_rounds,
    })
    print(f"[flow] skill_executor triggered: session={session_id} dialogue={dialogue_id}", file=sys.stderr)
    print(f"[flow]   exec_skill_key={exec_skill_key} skill_name={skill_name}", file=sys.stderr)
    print(f"[flow]   skill_prompt={skill_prompt[:80] if skill_prompt else '(none)'}", file=sys.stderr)
    print(f"[flow]   load_skills={load_skill_names}", file=sys.stderr)
    print(f"[flow]   tools_available={[t['function']['name'] for t in available_tools]}", file=sys.stderr)

    # Node-level timeout (injected by framework as AF_NODE_EXECUTION_TIMEOUT);
    # fallback to node default 240s if absent.
    node_timeout_sec = AgentConfig.node_execution_timeout()
    completion_budget = AgentConfig.skill_max_completion_tokens()
    recovery_attempts = 0
    max_recovery_attempts = AgentConfig.skill_llm_recovery_attempts()
    generated_artifacts = extract_artifact_paths(task_instruction)
    generated_artifacts = _ensure_artifacts_in_package(generated_artifacts)
    # tool_call_history: 每轮 tool_call 签名列表，用于检测 LLM 反复调同一 tool 同一参数的死循环
    tool_call_history: list[list[tuple[str, str]]] = []

    _turn_start = time.time()
    for round_i in range(max_rounds):
        elapsed = time.time() - _turn_start
        # ── 硬超时：node_timeout_sec 是边界约束，超过必须停（不变量 I-2）──
        # build_round_reminder 只做"提醒"，没有硬约束；之前节点跑了 3 分钟+ 不停就是这里漏了 break。
        if elapsed > node_timeout_sec:
            emit_event("skill_executor.node_timeout", {
                "round": round_i + 1,
                "max_rounds": max_rounds,
                "skill_name": skill_name,
                "elapsed_s": round(elapsed, 1),
                "node_timeout_sec": node_timeout_sec,
            }, level="WARN")
            print(f"[flow]   ⚠️  node_timeout reached at round={round_i+1} "
                  f"elapsed={elapsed:.0f}s > limit={node_timeout_sec}s", file=sys.stderr)
            try:
                databus.response_push(f"{skill_name} 节点超时，正在汇总…",
                                    type="progress", skill=skill_name,
                                    round=str(round_i+1), status="completed",
                                    source="skill_executor")
            except Exception as exc:
                print(f"[flow]   WARN: node_timeout progress push failed: {exc}", file=sys.stderr)
            response_text = messages[-1].get("content", "") if messages else ""
            output = {
                "skill_result": response_text,
                "status": "node_timeout",
                "skill_name": skill_name,
                "rounds_used": round_i + 1,
                "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
            }
            if databus.is_connected():
                result_json = json.dumps(output, ensure_ascii=True)
                databus.put("skill_result:skill-runner", result_json)
            print(json.dumps(output, ensure_ascii=False))
            return
        # ── Interrupt flag poll (design §3.3.1) ──────────────────────────
        # daemon 的 handle_dialogue_interrupt 在 cleanup 前写入 global sys flag
        # "/session/{sid}/sys/interrupt:{did}"。每轮顶部轮询，命中则写 interrupted
        # 结果给 coordinator 走正常 cleanup，自己 return。
        # 轮询失败（DataBus 抖动）不阻断主流程（I-INT-5）。
        if databus.is_connected():
            try:
                _flag = databus.get_global(f"interrupt:{dialogue_id}")
                if _flag == "true":
                    emit_event("skill_executor.interrupted", {
                        "round": round_i + 1,
                        "skill_name": skill_name,
                        "elapsed_s": round(time.time() - _turn_start, 1),
                    }, level="WARN")
                    print(f"[flow]   ⚠️  interrupted by DIALOGUE-INTERRUPT at round={round_i+1}", file=sys.stderr)
                    try:
                        databus.response_push(f"{skill_name} 已被取消",
                                            type="progress", skill=skill_name,
                                            status="completed", source="skill_executor")
                    except Exception as exc:
                        print(f"[flow]   WARN: interrupt progress push failed: {exc}", file=sys.stderr)
                    output = {
                        "skill_result": "对话已被取消",
                        "status": "interrupted",
                        "skill_name": skill_name,
                        "rounds_used": round_i + 1,
                        "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
                    }
                    if databus.is_connected():
                        databus.put("skill_result:skill-runner",
                                    json.dumps(output, ensure_ascii=True))
                    print(json.dumps(output, ensure_ascii=False))
                    return
            except Exception as exc:
                print(f"[flow]   WARN: interrupt flag poll failed: {exc}", file=sys.stderr)
        emit_event("skill_executor.llm_round_start", {
            "round": round_i + 1,
            "max_rounds": max_rounds,
            "messages": len(messages),
            "model": model_name,
            "tools": bool(available_tools),
            "budget": completion_budget,
            "elapsed_s": round(elapsed, 1),
        })
        print(
            f"[flow]   LLM round={round_i+1}/{max_rounds} msgs={len(messages)} "
            f"tools={bool(available_tools)} model={model_name} budget={completion_budget} "
            f"elapsed={elapsed:.0f}s",
            file=sys.stderr,
        )

        # ── Progress: push round info to frontend ──
        # "第 X 轮：调用 LLM…" → 用户看到"正在等 API"而非"卡了"
        _round_start = time.time()
        progress_msg = f"{skill_name} 第 {round_i+1}/{max_rounds} 轮：调用 LLM…"
        # 仅通过 response_push 推流式进度，不再写 DataBus response key
        # （response key 仅供 coordinator 最终输出使用，进度写入会污染最终结果）
        try:
            databus.response_push(progress_msg, type="progress",
                                  skill=skill_name, round=str(round_i+1),
                                  status="running", source="skill_executor")
        except Exception as exc:
            print(f"[flow]   WARN: llm_call progress push failed: {exc}", file=sys.stderr)

        # ── Per-round budget reminder (delegated to shared module) ──
        # always=True: 从第 1 轮就注入预算遥测（剩余轮次+已用/剩余时间+进度%），
        # 让 LLM 主动规划收敛而非只在最后 3 轮才察觉预算紧张。
        # 与本节点硬约束（node_timeout break + tool_call_loop detection）互为兜底：
        # 这里是软提示层，硬约束兜底在循环顶部。
        reminder = build_round_reminder(
            round_i=round_i,
            max_rounds=max_rounds,
            elapsed=elapsed,
            node_timeout_sec=node_timeout_sec,
            always=True,
            custom_hint="本轮必须输出：要么调用工具继续推进，要么直接给出 skill_result 文本结果交还 coordinator",
        )
        if reminder:
            messages.append(reminder)

        # ── Token budget: compress messages if exceeding threshold ──
        # compress_messages 内部按设计文档 §6 格式打印 [flow] compaction: 日志行
        model_max_tokens = get_model_max_tokens()
        compaction_threshold = get_compaction_threshold()
        budget = int(model_max_tokens * compaction_threshold)
        compressed, level = compress_messages(
            messages, budget,
            session_id=_session_id(),
            dialogue_id=_dialogue_id(),
            fold_dir=AgentConfig.fold_dir(),
            protect_last_rounds=AgentConfig.protect_last_rounds(),
            hard_compression_enabled=AgentConfig.hard_compression_enabled(),
            hard_compression_model=AgentConfig.hard_compression_model(),
            node_type="skill",
        )
        if level != "skip":
            messages = compressed

        raw = call_llm(
            messages, tools=available_tools,
            model=model_name,
            base_url=base_url,
            api_key=api_key,
            max_completion_tokens=completion_budget,
            stream=stream,
            source="skill_executor",
            skill=skill_name,
            round_i=round_i + 1,
            extra_body={
                "temperature": AgentConfig.skill_llm_temperature(),
                "top_p": AgentConfig.skill_llm_top_p(),
                "frequency_penalty": AgentConfig.skill_llm_frequency_penalty(),
                "presence_penalty": AgentConfig.skill_llm_presence_penalty(),
                "top_k": AgentConfig.skill_llm_top_k() or None,
                "stop": AgentConfig.skill_llm_stop() or None,
            })

        if raw.get("_llm_error"):
            err_msg = raw.get("error", "LLM call failed")
            emit_event("skill_executor.llm_error", {
                "round": round_i + 1,
                "error": err_msg,
                "elapsed_s": round(time.time() - _turn_start, 1),
            }, level="ERROR")
            print(f"[flow]   ⚠️  LLM error after call_llm internal retries: {err_msg}", file=sys.stderr)
            # Transport-layer retry is handled INSIDE call_llm (SSL/DNS/timeout).
            # If we reach here, call_llm has exhausted its retries — the network
            # is genuinely down. Do NOT rewrite messages (the conversation is
            # fine; the failure was transport-only). Just bail out with a
            # structured result so the framework can re-trigger later.
            output = {
                "skill_result": f"LLM call failed: {err_msg}",
                "status": "llm_error",
                "skill_name": skill_name,
                "rounds_used": round_i + 1,
                "error": err_msg,
                "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
            }
            if databus.is_connected():
                result_json = json.dumps(output, ensure_ascii=True)
                databus.put("skill_result:skill-runner", result_json)
            print(json.dumps(output, ensure_ascii=False))
            return

        # ── 检测 LLM 返回空内容（API 成功但未实际处理请求） ──
        _raw_usage = raw.get("usage", {}) or {}
        _raw_choice = raw.get("choices", [{}])[0]
        _raw_msg = _raw_choice.get("message", {})
        _raw_content = _raw_msg.get("content", "")
        _raw_finish = _raw_choice.get("finish_reason", "")
        _raw_tc = _raw_msg.get("tool_calls")
        _is_empty_response = (
            not _raw_content
            and not _raw_tc
            and _raw_finish == "stop"
            and _raw_usage.get("prompt_tokens", 0) == 0
            and _raw_usage.get("completion_tokens", 0) == 0
        )
        if _is_empty_response:
            err_msg = "LLM returned empty response (zero tokens, no content, finish=stop)"
            emit_event("skill_executor.llm_error", {
                "round": round_i + 1,
                "error": err_msg,
                "elapsed_s": round(time.time() - _turn_start, 1),
            }, level="ERROR")
            print(f"[flow]   ⚠️  {err_msg}", file=sys.stderr)
            print(f"[flow]   raw_response={json.dumps(raw, ensure_ascii=False)[:500]}", file=sys.stderr)
            output = {
                "skill_result": f"LLM call failed: {err_msg}",
                "status": "llm_error",
                "skill_name": skill_name,
                "rounds_used": round_i + 1,
                "error": err_msg,
                "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
            }
            if databus.is_connected():
                result_json = json.dumps(output, ensure_ascii=True)
                databus.put("skill_result:skill-runner", result_json)
            print(json.dumps(output, ensure_ascii=False))
            return

        choice = raw.get("choices", [{}])[0]
        msg = choice.get("message", {})
        tool_calls = msg.get("tool_calls")

        valid_tool_calls, invalid_reason = validate_tool_calls(choice)
        if not valid_tool_calls:
            # SEMANTIC recovery (truncated tool args / invalid JSON) — this is
            # the legitimate outer-loop recovery: rewrite messages to ask the
            # LLM for shorter, valid tool args. Distinct from transport retry.
            if recovery_attempts < max_recovery_attempts:
                recovery_attempts += 1
                completion_budget = min(completion_budget * 2, 16384)
                messages.append({
                    "role": "user",
                    "content": SKILL_RECOVERY_PROMPT,
                })
                print(
                    f"[flow]   recovering {invalid_reason} attempt="
                    f"{recovery_attempts}/{max_recovery_attempts} "
                    f"budget={completion_budget}",
                    file=sys.stderr,
                )
                try:
                    databus.response_push(f"{skill_name} 正在恢复第 {round_i+1} 轮…",
                                          type="progress", skill=skill_name,
                                          round=str(round_i+1), status="running",
                                          source="skill_executor")
                except Exception as exc:
                    print(f"[flow]   WARN: recover progress push failed: {exc}", file=sys.stderr)
                    continue

            output = {
                "skill_result": f"Tool call recovery exhausted: {invalid_reason}",
                "status": "tool_call_error",
                "skill_name": skill_name,
                "rounds_used": round_i + 1,
                "error": invalid_reason,
                "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
            }
            if databus.is_connected():
                result_json = json.dumps(output, ensure_ascii=True)
                databus.put("skill_result:skill-runner", result_json)
            print(json.dumps(output, ensure_ascii=False))
            return

        if tool_calls:
            tool_names = [tc.get("function", {}).get("name", "?") for tc in tool_calls]
            emit_event("skill_executor.tool_call", {
                "round": round_i + 1,
                "tool_names": tool_names,
                "tool_count": len(tool_calls),
            })
            print(f"[flow]   round={round_i+1} → tool_calls: {tool_names}", file=sys.stderr)
            # ── Progress: tool call details ──
            msg_tool = f"{skill_name} → 正在调用 {', '.join(tool_names)}"
            try:
                databus.response_push(msg_tool, type="progress",
                                      skill=skill_name, round=str(round_i+1),
                                      status="running", source="skill_executor")
            except Exception as exc:
                print(f"[flow]   WARN: tool_call progress push failed: {exc}", file=sys.stderr)
            messages.append(msg)
            # ── Runaway detection: track tool_call signatures to detect loops ──
            # LLM 陷入"反复调同一 tool 同一参数"时显式打断，避免耗光 max_rounds 才停。
            # 不变量 I-4 幂等性：同一规则重复应用不应产生重复效果——重复 tool_call 即违反。
            new_signatures = []
            for tc in tool_calls:
                func = tc.get("function", {})
                name = func.get("name", "")
                args = json.loads(func.get("arguments", "{}"))
                sig = (name, json.dumps(args, ensure_ascii=False, sort_keys=True))
                new_signatures.append(sig)
            # 连续 3 轮出现完全相同的 tool_call 签名 → 视为死循环，强制走完成分支
            if _detect_tool_call_loop(tool_call_history, new_signatures, threshold=3):
                emit_event("skill_executor.tool_call_loop_detected", {
                    "round": round_i + 1,
                    "skill_name": skill_name,
                    "signatures": new_signatures,
                    "elapsed_s": round(time.time() - _turn_start, 1),
                }, level="WARN")
                print(f"[flow]   ⚠️  tool_call loop detected at round={round_i+1}, forcing completion", file=sys.stderr)
                try:
                    databus.response_push(f"{skill_name} 检测到重复调用，正在汇总…",
                                          type="progress", skill=skill_name,
                                          status="completed", source="skill_executor")
                except Exception as exc:
                    print(f"[flow]   WARN: loop_detection progress push failed: {exc}", file=sys.stderr)
                # 强制走完成分支：把最后一轮 LLM 的 content（若有）当结果
                response_text = msg.get("content", "") or _summarize_tool_history(messages)
                output = {
                    "skill_result": response_text,
                    "status": "tool_call_loop_detected",
                    "skill_name": skill_name,
                    "rounds_used": round_i + 1,
                    "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
                }
                emit_event("skill_executor.completed", {
                    "round": round_i + 1,
                    "total_rounds": round_i + 1,
                    "skill_name": skill_name,
                    "status": output["status"],
                    "response_length": len(response_text),
                    "artifacts_count": len(generated_artifacts),
                    "elapsed_s": round(time.time() - _turn_start, 1),
                })
                print(f"[flow]   result: skill={skill_name} status=tool_call_loop_detected rounds={round_i+1}", file=sys.stderr)
                if databus.is_connected():
                    result_json = json.dumps(output, ensure_ascii=True)
                    databus.put("skill_result:skill-runner", result_json)
                print(json.dumps(output, ensure_ascii=False))
                return
            tool_call_history.append(new_signatures)
            for tc in tool_calls:
                func = tc.get("function", {})
                name = func.get("name", "")
                args = json.loads(func.get("arguments", "{}"))
                # Log tool call params (truncate to 200 chars)
                args_preview = json.dumps(args, ensure_ascii=False)[:200]
                print(f"[flow]   tool_call: {name} args={args_preview}", file=sys.stderr)
                result = tool_registry.execute(name, args)
                # Audit: artifacts collected from this tool result (完整清单，F7)
                tool_artifacts = extract_artifact_paths(result)
                new_count = 0
                for artifact in tool_artifacts:
                    if artifact not in generated_artifacts:
                        generated_artifacts.append(artifact)
                        new_count += 1
                # Relocate artifacts written outside agent package into package outputs/
                generated_artifacts = _ensure_artifacts_in_package(generated_artifacts)
                # ── F7: 不截断存储层。完整 tool_result 落 stderr ──
                # 通用路径必须默认完整（F7）；此处是 daemon 捕获 stderr 的主路径，
                # 完整内容应同时落独立文件兜底（F7.1），但 stderr 链路本身不截断。
                print(
                    f"[flow]   tool_result: {name} => {result}",
                    file=sys.stderr,
                )
                if tool_artifacts:
                    print(
                        f"[flow]   artifact.from_tool {name} collected={len(tool_artifacts)} "
                        f"new={new_count} paths=" + json.dumps(tool_artifacts, ensure_ascii=False),
                        file=sys.stderr,
                    )
                messages.append({
                    "role": "tool",
                    "tool_call_id": tc.get("id", name),
                    "content": result,
                })
        else:
            # Task complete → output result
            # ── Progress: LLM returned final answer ──
            msg_summary = f"{skill_name} 汇总结果…"
            try:
                databus.response_push(msg_summary, type="progress",
                                      skill=skill_name, status="completed",
                                      source="skill_executor")
            except Exception as exc:
                print(f"[flow]   WARN: summary progress push failed: {exc}", file=sys.stderr)
            response_text = msg.get("content", "")
            output = {
                "skill_result": response_text,
                "status": "completed",
                "skill_name": skill_name,
                "rounds_used": round_i + 1,
                "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
            }
            if not response_text.strip():
                emit_event("skill_executor.empty_result", {
                    "round": round_i + 1,
                    "skill_name": skill_name,
                    "tool_calls_in_round": bool(tool_calls),
                    "elapsed_s": round(time.time() - _turn_start, 1),
                }, level="WARN")
                print(f"[flow]   ⚠️  empty skill_result: skill={skill_name} "
                      f"LLM returned finish=stop with no content", file=sys.stderr)
            # Write result to DataBus for coordinator to read.
            #   skill_result:skill-runner — key matching AF_COMPLETED_SKILL_NAME
            # Coordinator's collect_skill_results reads by completed_skill name ("skill-runner"),
            # so this key MUST be written or coordinator gets empty result → false "not found".
            # Use ensure_ascii=True to avoid DataBus SDK UTF-8 decode failures on Chinese/emoji content.
            emit_event("skill_executor.completed", {
                "round": round_i + 1,
                "total_rounds": round_i + 1,
                "skill_name": skill_name,
                "status": output.get("status", "completed"),
                "response_length": len(response_text),
                "artifacts_count": len(generated_artifacts),
                "elapsed_s": round(time.time() - _turn_start, 1),
            })
            print(f"[flow]   result: skill={skill_name} status={output.get('status','completed')} rounds={round_i+1}", file=sys.stderr)
            if databus.is_connected():
                result_json = json.dumps(output, ensure_ascii=True)
                databus.put("skill_result:skill-runner", result_json)
            print(json.dumps(output, ensure_ascii=False))
            return

    # Max rounds reached
    emit_event("skill_executor.max_rounds_reached", {
        "max_rounds": max_rounds,
        "skill_name": skill_name,
        "response_length": len(messages[-1].get("content", "")) if messages else 0,
        "elapsed_s": round(time.time() - _turn_start, 1),
    }, level="WARN")
    output = {
        "skill_result": messages[-1].get("content", "") if messages else "",
        "status": "max_rounds_reached",
        "skill_name": skill_name,
        "rounds_used": max_rounds,
        "artifacts_json": json.dumps(generated_artifacts, ensure_ascii=False),
    }
    if databus.is_connected():
        result_json = json.dumps(output, ensure_ascii=True)
        databus.put("skill_result:skill-runner", result_json)
    print(json.dumps(output, ensure_ascii=False))


if __name__ == "__main__":
    execute()
