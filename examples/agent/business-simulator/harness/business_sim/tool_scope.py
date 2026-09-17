"""Package-local tool scope for the simulation's skill subprocesses."""
from agentflow_runtime.tool_registry import _LocalToolRegistry
from .contracts import SERVICES, contract


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
        self._tools = [tool for tool in self._tools
                       if tool.get("function", {}).get("name") in names]
        self._modules = {name: module for name, module in self._modules.items()
                         if name in names}
        self._mcp_bridges = {}
