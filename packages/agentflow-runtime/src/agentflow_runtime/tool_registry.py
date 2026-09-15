"""Local tool registry: loads domain tools from harness/tools/ and skill scripts/.

Split from skill_executor.py (2026-08-14).  All public symbols are re-exported
by skill_executor so existing ``from agentflow_runtime.skill_executor import X``
call sites continue to work unchanged.

Design constraints:
  - AGENT_PACKAGE_DIR remains the single source of truth in skill_executor.
    This module reads it lazily via ``_se.AGENT_PACKAGE_DIR``.
  - _get_builtin_tools_dir / _get_agent_tools_dir / _get_skills_base live in
    skill_executor; this module calls them via lazy import.
  - No business logic / log format / protocol changes — pure structural split.
"""

import importlib.util as _importlib_util
import json
import logging
import os
import re
import sys
import time

from agentflow_runtime.security import (
    find_secret_exposures,
    has_registered_secrets,
    redact_secrets,
    register_environment_secrets,
)

logger = logging.getLogger(__name__)

_CREDENTIAL_FILES = frozenset({
    ".env", ".mcp.json", ".netrc", ".npmrc", ".pypirc",
    "id_rsa", "id_dsa", "id_ecdsa", "id_ed25519",
})
_CREDENTIAL_SUFFIXES = (".pem", ".key", ".p12", ".pfx", ".jks", ".keystore")


def _safe_tool_result(value) -> str:
    safe_value, _ = redact_secrets(value)
    if isinstance(safe_value, str):
        return safe_value
    return json.dumps(safe_value, ensure_ascii=False, default=str)


def _blocked_tool_result(tool_name: str, reason: str, report=None) -> str:
    payload = {
        "error": reason,
        "status": "security_policy_blocked",
        "tool": tool_name,
    }
    if report is not None and report.applied:
        payload["security"] = report.to_dict()
    return json.dumps(payload, ensure_ascii=False)


def _is_protected_path(path, *, for_write: bool = False) -> bool:
    if not isinstance(path, str) or not path.strip():
        return False
    normalized = path.replace("\\", "/").rstrip("/")
    basename = normalized.rsplit("/", 1)[-1].lower()
    if (basename in _CREDENTIAL_FILES or basename.startswith(".env.")
            or basename.endswith(_CREDENTIAL_SUFFIXES)):
        return True
    if not for_write:
        return False
    lowered = f"/{normalized.lower().lstrip('./')}"
    return (
        basename in {"agent.md", "manifest.json", "manifest.jsonc"}
        or "/harness/tools/" in lowered
        or "/harness/hooks/" in lowered
        or ("/harness/skills/" in lowered and "/scripts/" in lowered)
    )


def _pkg_dir() -> str:
    """Read AGENT_PACKAGE_DIR from skill_executor (single source of truth).

    Lazy import avoids circular dependency at module-load time.
    """
    from agentflow_runtime import skill_executor as _se
    return _se.AGENT_PACKAGE_DIR


def _get_builtin_tools_dir() -> str:
    """Return path to the built-in system tools directory."""
    from agentflow_runtime import skill_executor as _se
    return _se._get_builtin_tools_dir()


def _get_agent_tools_dir() -> str:
    """Return path to the agent package's local harness/tools (override)."""
    from agentflow_runtime import skill_executor as _se
    return _se._get_agent_tools_dir()


def _get_skills_base() -> str:
    """Return path to the skills base directory."""
    from agentflow_runtime import skill_executor as _se
    return _se._get_skills_base()


class _LocalToolRegistry:
    """Loads domain tools from global harness/tools/ AND skill-specific scripts/.

    Global tools (harness/tools/*.py) provide atomic capabilities like bash,
    read_file, write_file, web_fetch — available to ALL skills.

    Skill-scoped tools (harness/skills/{skill_name}/scripts/*.py) provide
    domain-specific tools — only available to the owning skill.

    Each tool module must expose: TOOL_NAME, TOOL_DESCRIPTION, TOOL_PARAMS, execute(args).
    Global tools are loaded first; skill-scoped tools override same-named globals.
    """
    def __init__(self, skill_name: str = "", skill_names: list[str] | None = None):
        self._tools = []
        self._modules = {}
        self._mcp_bridges = {}  # server_name -> MCPBridge (I-5: per-instance)

        # Lazy import: tests monkeypatch skill_executor._load_dir, so we resolve
        # it via skill_executor's module namespace.
        from agentflow_runtime import skill_executor as _se
        _ld = _se._load_dir

        # 0. Inject AGENT_PACKAGE_DIR for built-in tools (bash, load_skill, etc.)
        os.environ["AF_AGENT_PACKAGE_DIR"] = _pkg_dir()

        # 1. Load built-in system tools from the agentflow-runtime package
        builtin_dir = _get_builtin_tools_dir()
        if os.path.isdir(builtin_dir):
            _ld(self, builtin_dir)

        # 2. Load agent-specific tools from agent package (override)
        agent_dir = _get_agent_tools_dir()
        if os.path.isdir(agent_dir) and os.path.abspath(agent_dir) != os.path.abspath(builtin_dir):
            _ld(self, agent_dir)

        # 3. Load primary + explicitly requested companion skill tools.
        selected_skills = []
        for selected in [skill_name, *(skill_names or [])]:
            if selected and selected not in selected_skills:
                selected_skills.append(selected)
        for selected in selected_skills:
            skill_dir = _se._resolve_skill_dir(selected)
            if skill_dir is not None:
                scripts_dir = os.path.realpath(os.path.join(skill_dir, "scripts"))
                if (os.path.dirname(scripts_dir) == skill_dir
                        and os.path.isdir(scripts_dir)):
                    _ld(self, scripts_dir, prefix=f"_skill_{selected}_")

        # 4. Load MCP server tools (追加，不阻塞，I-4 优雅降级)
        self._load_mcp_tools()

    def _load_mcp_tools(self):
        """加载 MCP server 工具并注册路由（I-4: 任何失败都降级不抛）。"""
        # Lazy import: tests monkeypatch skill_executor.{MCP_AVAILABLE, MCPBridge,
        # discover_servers, _load_dir}, so we resolve these via skill_executor's
        # module namespace.  Also use skill_executor's logger for caplog compatibility.
        from agentflow_runtime import skill_executor as _se
        _log = logging.getLogger("agentflow_runtime.skill_executor")
        try:
            if not _se.MCP_AVAILABLE:
                _log.warning("mcp_tools.skip mcp_sdk_missing")
                return

            # 收集配置路径：env 配置 + agent package .mcp.json + cwd/.mcp.json
            paths = []
            env_paths = os.getenv("AF_MCP_CONFIG_PATHS", "")
            if env_paths:
                paths.extend(p for p in env_paths.split(os.pathsep) if p)
            pkg_dir = os.environ.get("AF_AGENT_PACKAGE_DIR", _pkg_dir())
            paths.append(os.path.join(pkg_dir, ".mcp.json"))
            paths.append(os.path.join(os.getcwd(), ".mcp.json"))
            # 去重
            seen = set()
            deduped = []
            for p in paths:
                ap = os.path.abspath(p)
                if ap not in seen:
                    seen.add(ap)
                    deduped.append(p)

            # 入口日志：标明开始扫配置 + 扫哪些路径（排障时区分"没跑到这段"vs"跑到了但配置为空"）
            _log.info("mcp_tools.start scan_paths=%s", deduped)

            configs = _se.discover_servers(*deduped)
            if not configs:
                # 配置为空也要打印扫的路径——排障可定位"路径算错"还是"路径对但文件不存在"
                _log.info("mcp_tools.none_config scan_paths=%s", deduped)
                return

            for cfg in configs:
                try:
                    bridge = _se.MCPBridge(cfg)
                    if not bridge.ensure_ready():
                        _log.warning("mcp_tools.skip_connect server=%s", cfg.name)
                        continue
                    tools = bridge.load_tools()
                    for tool in tools:
                        # 仅并入 get_tools() 使 LLM 可见；execute 路由由 mcp__ 前缀判断单一接管，
                        # 不注册 _modules（避免双轨路由：若未来前缀判断后移，dict 值会被当模块调用）
                        self._tools.append(tool)
                    self._mcp_bridges[cfg.name] = bridge
                    _log.info("mcp_tools.loaded server=%s count=%d",
                                 cfg.name, len(tools))
                except Exception as err:
                    _log.warning("mcp_tools.skip_connect server=%s error=%s",
                                    cfg.name, err)
                    continue
        except Exception as e:
            logger.warning("mcp_tools.load_failed error=%s", e)

    def get_tools(self):
        return list(self._tools)

    def execute(self, name, args):
        register_environment_secrets()
        args = args or {}
        exposure = find_secret_exposures(args)
        if exposure.applied:
            return _blocked_tool_result(
                str(name), "tool arguments contain secret material", exposure)

        sandbox_mode = os.environ.get("AF_SANDBOX_MODE", "none")
        allow_unsafe_local = os.environ.get(
            "AF_ALLOW_UNSAFE_LOCAL_SECRET_TOOLS", ""
        ).strip().lower() in {"1", "true", "yes"}
        if (name == "bash" and sandbox_mode == "none"
                and has_registered_secrets() and not allow_unsafe_local):
            return _blocked_tool_result(
                str(name),
                f"bash is disabled in {sandbox_mode} mode while credentials are loaded",
            )

        if name in {"read_file", "write_file", "edit_file"}:
            file_path = args.get("file_path", "") if isinstance(args, dict) else ""
            if _is_protected_path(
                    file_path, for_write=name in {"write_file", "edit_file"}):
                return _blocked_tool_result(
                    str(name), "access to credential or executable configuration is denied")

        # I-6: MCP 路由在 sandbox 分支之前，保证 MCP 工具永不误走沙箱 /exec
        if name.startswith("mcp__"):
            logger.info("tool.exec sandbox_mode=mcp tool=%s", name)
            parts = name.split("__")  # mcp__{server}__{tool}
            server_name, tool_name = parts[1], "__".join(parts[2:])
            bridge = self._mcp_bridges.get(server_name)
            if bridge is None:
                return _safe_tool_result(
                    {"error": f"MCP server '{server_name}' not connected"})
            try:
                result = bridge.call_tool(tool_name, args or {})
                return _safe_tool_result(result)
            except Exception as e:
                return _safe_tool_result(
                    {"error": f"MCP tool '{name}' failed: {e}"})

        _mod = self._modules.get(name)
        if _mod is None:
            return _safe_tool_result({"error": f"Unknown tool: {name}"})
        _t_exec_start = time.monotonic()
        try:
            # 由 daemon 注入的沙箱 HTTP endpoint（B16），bash 等工具走沙箱 /exec
            # 独立于 AF_ARTIFACT_STORAGE_URL（产物上传用），避免 env var 重载
            sandbox_endpoint = os.environ.get("AF_SANDBOX_ENDPOINT", "")
            sandbox_mode = os.environ.get("AF_SANDBOX_MODE", "none")
            if sandbox_endpoint:
                # 显式打印 mode 值——日志一眼看出走哪条执行路径
                # （e2b→E2BExecutor 远端沙箱，local/docker→HTTPExecutor，none→HostExecutor 本地）
                logger.info(
                    "tool.exec sandbox_mode=%s tool=%s sandbox_id=%s",
                    sandbox_mode, name,
                    sandbox_endpoint.split('/')[-2] if '/sandbox/' in sandbox_endpoint else '?')
            else:
                logger.info("tool.exec sandbox_mode=%s tool=%s (no endpoint, host fallback)",
                             sandbox_mode, name)
            # 沙箱 endpoint 为空时，工具走本地 subprocess.run 路径
            result = _mod.execute(args, sandbox_endpoint=sandbox_endpoint)
            _dur_ms = (time.monotonic() - _t_exec_start) * 1000
            print(f"[timing] tool_exec name={name} duration_ms={_dur_ms:.1f} sandbox_mode={sandbox_mode}",
                  file=sys.stderr)
            return _safe_tool_result(result)
        except Exception as e:
            _dur_ms = (time.monotonic() - _t_exec_start) * 1000
            print(f"[timing] tool_exec name={name} duration_ms={_dur_ms:.1f} error=1", file=sys.stderr)
            return _safe_tool_result({"error": f"Tool '{name}' failed: {e}"})


def _load_dir(registry: _LocalToolRegistry, directory: str, prefix: str = ""):
    """Load all valid tool modules from a directory into the registry."""
    directory = os.path.realpath(directory)
    for entry in sorted(os.listdir(directory)):
        if not entry.endswith(".py") or entry.startswith("_"):
            continue
        mod_name = entry[:-3]
        entry_path = os.path.realpath(os.path.join(directory, entry))
        if os.path.dirname(entry_path) != directory:
            logger.warning("tool_loader.skip module=%s error=path_outside_tool_dir", mod_name)
            continue

        try:
            spec = _importlib_util.spec_from_file_location(
                f"_tool_{prefix}{mod_name}",
                entry_path)
            if spec is None or spec.loader is None:
                continue
            _mod = _importlib_util.module_from_spec(spec)
            spec.loader.exec_module(_mod)
        except Exception as exc:
            logger.warning("tool_loader.skip module=%s error=%s", mod_name, exc)
            continue
        if not all(hasattr(_mod, a) for a in ("TOOL_NAME", "TOOL_DESCRIPTION", "TOOL_PARAMS", "execute")):
            continue
        tool_name = _mod.TOOL_NAME
        tool_schema = {"type": "function", "function": {
            "name": tool_name,
            "description": _mod.TOOL_DESCRIPTION,
            "parameters": _mod.TOOL_PARAMS,
        }}
        registry._modules[tool_name] = _mod
        for index, registered in enumerate(registry._tools):
            if registered.get("function", {}).get("name") == tool_name:
                registry._tools[index] = tool_schema
                break
        else:
            registry._tools.append(tool_schema)


# ---------------------------------------------------------------------------
# agent.md frontmatter env expansion helper
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
