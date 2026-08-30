"""HTTP Sandbox web_fetch dispatch and response-contract tests."""

import json
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import sandbox_http_server as server


class TestWebFetchDispatch(unittest.TestCase):
    def test_web_fetch_is_registered(self):
        self.assertIs(server.TOOL_DISPATCH["web_fetch"], server._exec_tool)

    def test_web_fetch_content_is_normalized_to_stdout(self):
        module = SimpleNamespace(
            execute=lambda _tool_input: json.dumps(
                {"content": "public page", "error": None, "total_lines": 1}
            )
        )
        with patch.object(server.importlib, "import_module", return_value=module):
            result = server._exec_tool(
                "web_fetch",
                {"url": "https://public.example/page"},
                {"timeout_ms": 1000},
            )

        self.assertEqual(result["exit_code"], 0)
        self.assertEqual(result["stdout"], "public page")
        self.assertEqual(result["stderr"], "")

    def test_web_fetch_error_remains_a_failure(self):
        module = SimpleNamespace(
            execute=lambda _tool_input: json.dumps({"error": "network denied"})
        )
        with patch.object(server.importlib, "import_module", return_value=module):
            result = server._exec_tool(
                "web_fetch",
                {"url": "https://public.example/page"},
                {"timeout_ms": 1000},
            )

        self.assertEqual(result["exit_code"], 1)
        self.assertIn("network denied", result["stderr"])


if __name__ == "__main__":
    unittest.main()
