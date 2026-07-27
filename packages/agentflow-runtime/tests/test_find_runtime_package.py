"""Tests for find_runtime_package_path() — site-packages location strategy (T-001)."""

import subprocess
from unittest.mock import MagicMock, patch

import pytest

from agentflow_runtime.common.find_runtime_package import (
    _TEST_FAKE_PATH,
    find_runtime_package_path,
)

_SKIP = "agentflow_runtime.common.find_runtime_package._TEST_SKIP_IMPORT"
_IMPORT = "agentflow_runtime.common.find_runtime_package.importlib.import_module"
_SUBRUN = "agentflow_runtime.common.find_runtime_package.subprocess.run"


def test_find_by_import():
    """Import succeeds -> returns __path__[0]."""
    with patch(_SKIP, False):
        with patch(
            _IMPORT,
            return_value=MagicMock(__path__=["/fake/site-packages/agentflow_runtime"]),
        ):
            assert find_runtime_package_path() == "/fake/site-packages/agentflow_runtime"


def test_find_by_import_trailing_slash():
    """Trailing slash is normalized away."""
    with patch(_SKIP, False):
        with patch(_IMPORT, return_value=MagicMock(__path__=["/fake/pkg/"])):
            assert find_runtime_package_path() == "/fake/pkg"


def test_fallback_to_pip_show():
    """Import skipped -> fall back to pip show Location: field."""
    pip_output = "Name: agentflow-runtime\nLocation: /path/to/site-packages\n"
    with patch(_SKIP, True):
        with patch(_SUBRUN, return_value=MagicMock(stdout=pip_output, returncode=0)):
            assert find_runtime_package_path() == "/path/to/site-packages/agentflow_runtime"


def test_pip_show_strips_newline():
    """Location: line may have trailing newline, result is clean."""
    pip_output = "Name: agentflow-runtime\nLocation: /usr/lib/py3.12/site-packages\n"
    with patch(_SKIP, True):
        with patch(_SUBRUN, return_value=MagicMock(stdout=pip_output, returncode=0)):
            assert (
                find_runtime_package_path()
                == "/usr/lib/py3.12/site-packages/agentflow_runtime"
            )


def test_pip_not_installed():
    """All strategies fail -> raise RuntimeError."""
    with patch(_SKIP, True):
        with patch(_SUBRUN, side_effect=FileNotFoundError("pip not found")):
            with pytest.raises(RuntimeError, match="Cannot find agentflow_runtime"):
                find_runtime_package_path()


def test_pip_fails_nonzero():
    """pip show returns non-zero -> raise RuntimeError."""
    with patch(_SKIP, True):
        with patch(
            _SUBRUN,
            side_effect=subprocess.CalledProcessError(1, ["pip", "show"]),
        ):
            with pytest.raises(RuntimeError, match="Cannot find agentflow_runtime"):
                find_runtime_package_path()
