"""Check the package adapter against the real runtime's dynamic skill loader."""
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE / "harness"))


class ToolScopeTests(unittest.TestCase):
    def test_companion_business_tools_load_without_general_execution_tools(self):
        try:
            from agentflow_runtime import skill_executor as runtime
            from business_sim.tool_scope import BusinessToolRegistry
        except ImportError:
            self.skipTest("requires installed agentflow-runtime (Python >=3.12)")
        original = runtime._LocalToolRegistry
        with patch.object(runtime, "AGENT_PACKAGE_DIR", str(PACKAGE)), patch.object(runtime, "MCP_AVAILABLE", False):
            registry = BusinessToolRegistry("health-advice", ["smart-home"])
            names = {tool["function"]["name"] for tool in registry.get_tools()}
            self.assertEqual(names, {"sim_health_advice", "sim_smart_home"})
            self.assertNotIn("bash", registry._modules)
            self.assertNotIn("read_file", registry._modules)
            result = registry.execute("bash", {"command": "echo must-not-execute"})
            if isinstance(result, str):
                result = json.loads(result)
            self.assertTrue(result.get("error"), result)
        self.assertIs(runtime._LocalToolRegistry, original)


if __name__ == "__main__":
    unittest.main()
