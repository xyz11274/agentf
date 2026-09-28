"""BFCL package rewrites must remain isolated at production concurrency."""
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/trajectory_evolution/experiments/bfcl-v3"))
from bfcl_v3_host import AGENT_PACKAGE, AgentRunner

WORKFLOWS = ("coordinator_workflow.json", "dummy_pre_skill_workflow.json", "skill_workflow.json")


class IsolationTest(unittest.TestCase):
    def test_sixteen_packages_never_write_shared_workflows(self):
        before = {name: (AGENT_PACKAGE / name).read_bytes() for name in WORKFLOWS}
        runners = []
        try:
            with ThreadPoolExecutor(max_workers=16) as pool:
                runners = list(pool.map(lambda _: AgentRunner(), range(16)))
            self.assertEqual(len({str(r.agent_package) for r in runners}), 16)
            for runner in runners:
                for name in WORKFLOWS:
                    path = runner.agent_package / name
                    self.assertFalse(path.is_symlink())
                    document = json.loads(path.read_text())
                    for node in document["nodes"]:
                        self.assertEqual(node["working_dir"], str(runner.agent_package))
            self.assertEqual(before, {name: (AGENT_PACKAGE / name).read_bytes() for name in WORKFLOWS})
        finally:
            for runner in runners:
                runner.close()


if __name__ == "__main__":
    unittest.main()
