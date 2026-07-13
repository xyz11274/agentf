#!/usr/bin/env python3
"""Unit tests for agent-creator tools — pure logic, no LLM/DataBus dependency."""
import json
import os
import sys
import unittest

# Ensure tools directory is on path so tests can import tool modules directly
_TOOLS_DIR = os.path.join(os.path.dirname(__file__), "..", "tools")
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)

# Clean slate before each test
def setUpModule():
    import file_store
    file_store.files.clear()
    file_store.finalized = False


class TestAddFile(unittest.TestCase):
    def setUp(self):
        from file_store import files
        files.clear()

    def test_add_new_file(self):
        from add_file import execute
        result = json.loads(execute({"path": "agent.json", "content": "{}"}))
        self.assertEqual(result["action"], "added")
        self.assertEqual(result["path"], "agent.json")
        self.assertEqual(result["size"], 2)

    def test_add_then_update(self):
        from add_file import execute
        execute({"path": "test.json", "content": "v1"})
        result = json.loads(execute({"path": "test.json", "content": "v2"}))
        self.assertEqual(result["action"], "updated")
        self.assertEqual(result["size"], 2)

    def test_add_empty_path_returns_error(self):
        from add_file import execute
        result = json.loads(execute({"path": "", "content": "x"}))
        self.assertIn("error", result)

    def test_add_no_params_returns_error(self):
        from add_file import execute
        result = json.loads(execute(None))
        self.assertIn("error", result)

    def test_add_multiple_files(self):
        from add_file import execute
        from file_store import files
        for name in ["a.json", "b.json", "c.json"]:
            execute({"path": name, "content": "{}"})
        self.assertEqual(len(files), 3)
        for name in ["a.json", "b.json", "c.json"]:
            self.assertIn(name, files)


class TestDeleteFile(unittest.TestCase):
    def setUp(self):
        from file_store import files
        files.clear()
        files["keep.json"] = "{}"
        files["remove.json"] = "data"

    def test_delete_existing(self):
        from delete_file import execute
        result = json.loads(execute({"path": "remove.json"}))
        self.assertEqual(result["action"], "deleted")
        from file_store import files
        self.assertNotIn("remove.json", files)

    def test_delete_nonexistent(self):
        from delete_file import execute
        result = json.loads(execute({"path": "ghost.json"}))
        self.assertEqual(result["action"], "not_found")

    def test_delete_empty_path_returns_error(self):
        from delete_file import execute
        result = json.loads(execute({"path": ""}))
        self.assertIn("error", result)

    def test_delete_no_params_returns_error(self):
        from delete_file import execute
        result = json.loads(execute(None))
        self.assertIn("error", result)


class TestFinalize(unittest.TestCase):
    def setUp(self):
        import file_store
        file_store.files.clear()
        file_store.files["agent.json"] = "{}"
        file_store.finalized = False

    def test_finalize_marks_done(self):
        from finalize import execute, is_finalized
        self.assertFalse(is_finalized())
        result = json.loads(execute({}))
        self.assertTrue(result["ok"])
        self.assertEqual(result["file_count"], 1)
        self.assertTrue(is_finalized())

    def test_finalize_reports_all_files(self):
        from finalize import execute
        from file_store import files
        files["extra.md"] = "# hi"
        result = json.loads(execute({}))
        self.assertEqual(result["file_count"], 2)
        self.assertIn("agent.json", result["files"])
        self.assertIn("extra.md", result["files"])

    def test_finalize_is_idempotent(self):
        from finalize import execute, is_finalized
        execute({})
        self.assertTrue(is_finalized())
        # second call should still succeed
        result2 = json.loads(execute({}))
        self.assertTrue(result2["ok"])


class TestFileStoreSharedState(unittest.TestCase):
    """Verify that all tools share the same ``file_store.files`` dict."""

    def test_add_then_delete_via_shared_state(self):
        from file_store import files
        from add_file import execute as add
        from delete_file import execute as delete

        add({"path": "shared.txt", "content": "hello"})
        self.assertIn("shared.txt", files)

        delete({"path": "shared.txt"})
        self.assertNotIn("shared.txt", files)


if __name__ == "__main__":
    unittest.main()
