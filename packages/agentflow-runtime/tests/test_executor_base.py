"""ToolExecutor interface contract tests.

Tests the abstract interface and dataclass contracts defined in ADR
20260724-tools-runtime-refactor.md.

These tests verify the *shape* of the interface, not any specific provider's
behavior.  Provider behavior is tested in test_host_executor.py etc.
"""

import pytest

from agentflow_runtime.executors.base import (
    ExecResult,
    FileContent,
    ToolExecutor,
    WriteResult,
)


# ---------------------------------------------------------------------------
# ToolExecutor ABC contract
# ---------------------------------------------------------------------------

def test_tool_executor_cannot_be_instantiated_directly():
    """ToolExecutor is abstract — direct instantiation must fail."""
    with pytest.raises(TypeError):
        ToolExecutor()


def test_tool_executor_requires_all_six_methods():
    """A partial subclass missing any of the 6 methods must also be abstract."""

    class _Partial(ToolExecutor):
        def exec_command(self, command, timeout=30, workdir=None):
            return ExecResult()

    with pytest.raises(TypeError):
        _Partial()


def test_tool_executor_full_impl_can_instantiate():
    """A concrete subclass implementing all 6 methods instantiates fine."""

    class _Full(ToolExecutor):
        def exec_command(self, command, timeout=30, workdir=None):
            return ExecResult()

        def read_file(self, path, offset=0, limit=0):
            return FileContent()

        def write_file(self, path, content):
            return WriteResult()

        def edit_file(self, path, old_str, new_str, replace_all=False):
            return ExecResult()

        def list_dir(self, path, recursive=False):
            return []

        def fetch_url(self, url, timeout=30):
            return FileContent()

    instance = _Full()
    assert isinstance(instance, ToolExecutor)


# ---------------------------------------------------------------------------
# ExecResult dataclass
# ---------------------------------------------------------------------------

def test_exec_result_default_fields():
    r = ExecResult()
    assert r.stdout == ""
    assert r.stderr == ""
    assert r.exit_code == 0
    assert r.error is None
    assert r.truncated is False


def test_exec_result_to_dict_keys():
    d = ExecResult(stdout="hi", exit_code=2).to_dict()
    assert d["stdout"] == "hi"
    assert d["exit_code"] == 2
    assert d["stderr"] == ""
    assert d["error"] is None
    assert d["truncated"] is False


def test_exec_result_to_dict_json_serializable():
    """to_dict() output must be JSON-serializable for tool layer."""
    import json

    r = ExecResult(stdout="out", stderr="err", exit_code=1, error="boom", truncated=True)
    s = json.dumps(r.to_dict())
    restored = json.loads(s)
    assert restored["stdout"] == "out"
    assert restored["error"] == "boom"


# ---------------------------------------------------------------------------
# FileContent dataclass
# ---------------------------------------------------------------------------

def test_file_content_default_fields():
    fc = FileContent()
    assert fc.content == ""
    assert fc.error is None
    assert fc.total_lines == 0


def test_file_content_to_dict_json_serializable():
    import json

    fc = FileContent(content="hello\nworld", total_lines=2)
    d = fc.to_dict()
    s = json.dumps(d)
    restored = json.loads(s)
    assert restored["content"] == "hello\nworld"
    assert restored["total_lines"] == 2


# ---------------------------------------------------------------------------
# WriteResult dataclass
# ---------------------------------------------------------------------------

def test_write_result_default_fields():
    wr = WriteResult()
    assert wr.success is False
    assert wr.error is None
    assert wr.bytes_written == 0


def test_write_result_to_dict_json_serializable():
    import json

    wr = WriteResult(success=True, bytes_written=42)
    d = wr.to_dict()
    s = json.dumps(d)
    restored = json.loads(s)
    assert restored["success"] is True
    assert restored["bytes_written"] == 42
