"""Package-local tool scope for the simulation's skill subprocesses."""
from agentflow_runtime.tool_registry import _LocalToolRegistry
from .contracts import SERVICES, contract
import json
import os
from pathlib import Path


class BusinessToolRegistry(_LocalToolRegistry):
    """Keep the runtime loader/executor, exposing only loaded business contracts.

    A simulation skill has no reason to run shell commands, edit source files,
    or load arbitrary additional skills. Companion business skills requested by
    the coordinator are preserved. This adapter is installed only by this
    package's skill node; other agents keep their original tool registry.
    """

    def __init__(self, skill_name="", skill_names=None):
        super().__init__(skill_name, skill_names)
        names = {contract(skill)[0] for skill in SERVICES}
        allowed = set()
        state_path = os.environ.get("BFCL_RUNTIME_STATE", "")
        state = {}
        if state_path:
            try:
                state = json.loads(Path(state_path).read_text(encoding="utf-8"))
                allowed = {str(name) for name in state.get("available_names", [])}
            except (OSError, ValueError, TypeError):
                state = {}
                allowed = set()
        # BFCL trials inject one private scripts directory per daemon.  Loading
        # it here preserves the production coordinator/skill-runner path while
        # keeping generated schemas out of the checked-in business package.
        dynamic_dir = os.environ.get("BFCL_RUNTIME_SKILL_DIR", "") or state.get("scripts_dir", "")
        # Older BFCL host revisions publish only row_file/available_names.  A
        # private fallback keeps those hosts compatible without changing the
        # AgentFlow runtime: generate sanitized modules beside the state file.
        if not dynamic_dir and state.get("row_file") and state_path:
            try:
                row = json.loads(Path(state["row_file"]).read_text(encoding="utf-8"))
                from bfcl_skill import _write_modules, available_functions
                dynamic_dir = str(Path(state_path).parent / "bfcl-functions" / "scripts")
                _write_modules(Path(dynamic_dir), available_functions(row, int(state.get("turn", 0))))
            except (OSError, ValueError, TypeError, ImportError, KeyError):
                dynamic_dir = ""
        if dynamic_dir:
            try:
                from agentflow_runtime import skill_executor as runtime
                if Path(dynamic_dir).is_dir():
                    runtime._load_dir(self, dynamic_dir, prefix="_bfcl_")
            except Exception:
                # A missing or malformed trial tool must degrade to an empty
                # BFCL skill; the surrounding task remains recoverable.
                pass
        names.update(allowed)
        self._tools = [tool for tool in self._tools
                       if tool.get("function", {}).get("name") in names]
        self._modules = {name: module for name, module in self._modules.items()
                         if name in names}
        self._mcp_bridges = {}
