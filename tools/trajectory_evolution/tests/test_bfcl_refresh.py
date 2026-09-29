import importlib.util
from pathlib import Path
import unittest


SKILL_PATH = Path(__file__).parents[3] / "examples/agent/business-simulator/harness/bfcl_skill.py"
spec = importlib.util.spec_from_file_location("bfcl_skill", SKILL_PATH)
bfcl_skill = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(bfcl_skill)


class BfclRefreshTest(unittest.TestCase):
    def setUp(self):
        self.row = {
            "id": "refresh",
            "functions": [{"name": "initial", "parameters": {"type": "object"}}],
            "missed_functions": {"1": [[{"name": "added", "parameters": {"type": "object"}}]]},
            "turns": [[{"role": "user", "content": "Book a flight"}], []],
        }

    def test_empty_turn_retains_public_request_and_adds_function(self):
        self.assertIn("Book a flight", bfcl_skill._request_text(self.row, 1))
        self.assertIn("function refresh", bfcl_skill._request_text(self.row, 1))
        self.assertEqual([f["name"] for f in bfcl_skill.available_functions(self.row, 1)],
                         ["initial", "added"])

    def test_refresh_request_does_not_include_ground_truth(self):
        self.row["ground_truth"] = [["secret_checker_answer"]]
        self.assertNotIn("secret_checker_answer", bfcl_skill._request_text(self.row, 1))


if __name__ == "__main__":
    unittest.main()
