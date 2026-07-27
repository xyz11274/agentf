"""Utility to locate the agentflow-runtime package in site-packages (T-001)."""

import importlib
import os
import subprocess
import sys

# Test hooks — set in tests to bypass real import/network.
_TEST_FAKE_PATH: str | None = None
_TEST_SKIP_IMPORT: bool = False


def find_runtime_package_path() -> str:
    """Return the filesystem path of the installed ``agentflow_runtime`` package."""
    if _TEST_FAKE_PATH is not None:
        return _TEST_FAKE_PATH

    # Strategy 1: use importlib (already loaded in this process)
    if not _TEST_SKIP_IMPORT:
        try:
            mod = importlib.import_module("agentflow_runtime")
            if mod.__path__:
                return mod.__path__[0].rstrip("/\\")
        except (ImportError, AttributeError, IndexError):
            pass

    # Strategy 2: ask pip
    try:
        result = subprocess.run(
            [sys.executable, "-m", "pip", "show", "agentflow-runtime"],
            capture_output=True,
            text=True,
            check=True,
        )
        for line in result.stdout.splitlines():
            if line.startswith("Location:"):
                path = line.split(":", 1)[1].strip()
                return os.path.join(path, "agentflow_runtime")
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass

    raise RuntimeError(
        "Cannot find agentflow_runtime package — "
        "is it installed in the current Python environment?"
    )
