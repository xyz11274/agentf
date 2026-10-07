"""Model-free checks for AppWorld's single lifecycle entry point."""
from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve()
APPWORLD = HERE.parents[1] / "appworld"
if str(APPWORLD) not in sys.path:
    sys.path.insert(0, str(APPWORLD))
import reproduce  # noqa: E402


class AppWorldSharedEntryTest(unittest.TestCase):
    def test_mutating_stage_flag_selects_shared_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            calls = []

            def fake_common(seed, label, *, stop_at=None):
                calls.append((Path(seed), label, stop_at))
                return {"status": "complete", "phase": stop_at}

            with patch.object(reproduce, "OUTPUT", Path(directory)), \
                 patch.object(reproduce, "_run_common_all", side_effect=fake_common), \
                 patch.object(sys, "argv", ["reproduce.py", "--validation100"]):
                self.assertEqual(reproduce.main(), 0)

            self.assertEqual(len(calls), 1)
            self.assertEqual(calls[0][2], "validation100")


if __name__ == "__main__":
    unittest.main()
