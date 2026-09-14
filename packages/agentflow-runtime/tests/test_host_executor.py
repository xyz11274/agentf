"""HostExecutor behavior tests — locks down existing host-path tool behavior.

Reference: ADR 20260724-tools-runtime-refactor.md Phase 1 characteristic tests.
Each test maps to a behavior in the existing bash.py/read_file.py/write_file.py/
edit_file.py host-mode paths (sandbox_endpoint=None).
"""

import os
from unittest.mock import MagicMock, patch

import pytest

from agentflow_runtime.executors.base import ExecResult, FileContent, WriteResult
from agentflow_runtime.executors.host import HostExecutor


@pytest.fixture
def executor(monkeypatch):
    # Characteristic command tests explicitly opt into host execution.
    monkeypatch.setenv("AF_ALLOW_UNSAFE_LOCAL_SECRET_TOOLS", "1")
    return HostExecutor()


# ---------------------------------------------------------------------------
# exec_command
# ---------------------------------------------------------------------------

def test_exec_echo_hello(executor):
    """exec_command('echo hello') returns stdout='hello\\n', exit_code=0."""
    result = executor.exec_command("echo hello")
    assert isinstance(result, ExecResult)
    assert result.stdout == "hello\n"
    assert result.exit_code == 0


def test_exec_exit_code_nonzero(executor):
    """exec_command('exit 1') returns exit_code=1."""
    result = executor.exec_command("exit 1")
    assert result.exit_code == 1


def test_exec_stderr_captured(executor):
    """stderr is captured when command writes to fd 2."""
    result = executor.exec_command("echo err_msg >&2")
    assert result.stderr.strip() == "err_msg"


def test_exec_workdir_parameter(executor, tmp_path):
    """workdir parameter changes the CWD of the subprocess."""
    result = executor.exec_command("pwd", workdir=str(tmp_path))
    # On macOS, /tmp is a symlink to /private/tmp — resolve for comparison
    resolved_tmp = os.path.realpath(str(tmp_path))
    resolved_stdout = os.path.realpath(result.stdout.strip())
    assert resolved_stdout == resolved_tmp


def test_exec_default_cwd_falls_back_to_agent_package_dir(executor, monkeypatch, tmp_path):
    """HostExecutor 兜底 AF_AGENT_PACKAGE_DIR——防御性 fallback。

    bash.py 缺省 cwd 透传宿主 agent_package_dir（非 None），此 fallback 在生产
    走不进去；保留兜底应对未来其他调用方传 None。本测试直接调 executor 验证兜底语义。
    """
    monkeypatch.setenv("AF_AGENT_PACKAGE_DIR", str(tmp_path))
    result = executor.exec_command("pwd")
    resolved_tmp = os.path.realpath(str(tmp_path))
    resolved_stdout = os.path.realpath(result.stdout.strip())
    assert resolved_stdout == resolved_tmp


def test_exec_timeout_returns_error(executor):
    """Timeout produces ExecResult with error containing timeout info."""
    result = executor.exec_command("sleep 10", timeout=1)
    assert result.exit_code != 0 or result.error is not None
    # The error message should mention timeout
    assert "timed out" in (result.error or "").lower() or result.exit_code != 0


# ---------------------------------------------------------------------------
# read_file
# ---------------------------------------------------------------------------

def test_read_file_known_content(executor, tmp_path):
    """read_file returns FileContent with correct content."""
    f = tmp_path / "test.txt"
    f.write_text("line1\nline2\nline3\n", encoding="utf-8")

    result = executor.read_file(str(f))
    assert isinstance(result, FileContent)
    assert result.error is None
    assert result.content == "line1\nline2\nline3\n"
    assert result.total_lines == 3


def test_read_file_nonexistent_returns_error(executor, tmp_path):
    """read_file on missing file returns FileContent with error containing path."""
    missing = str(tmp_path / "nope.txt")
    result = executor.read_file(missing)
    assert result.error is not None
    assert missing in result.error or "nope.txt" in result.error


def test_read_file_offset_limit(executor, tmp_path):
    """offset/limit parameters return only the selected line range."""
    f = tmp_path / "lines.txt"
    f.write_text("L1\nL2\nL3\nL4\nL5\n", encoding="utf-8")

    # Read lines 2-3 (1-based offset=2, limit=2)
    result = executor.read_file(str(f), offset=2, limit=2)
    assert result.content == "L2\nL3\n"
    assert result.total_lines == 5


def test_read_file_default_reads_all(executor, tmp_path):
    """offset=0, limit=0 reads the entire file."""
    f = tmp_path / "full.txt"
    f.write_text("only line\n", encoding="utf-8")

    result = executor.read_file(str(f))
    assert result.content == "only line\n"


@pytest.mark.parametrize("offset,limit", [(0, 0), (2, 1)])
def test_read_file_rejects_binary_even_with_line_selection(executor, tmp_path, offset, limit):
    """Binary headers are rejected before selecting apparently textual lines."""
    f = tmp_path / "image.png"
    f.write_bytes(b"\x89PNG\x00\nseemingly textual line\n")
    result = executor.read_file(str(f), offset=offset, limit=limit)
    assert result.content == ""
    assert "Binary content detected" in result.error


def test_read_file_rejects_nul_after_probe(executor, tmp_path):
    f = tmp_path / "payload.dat"
    f.write_bytes(b"a" * 9000 + b"\x00private binary payload")
    result = executor.read_file(str(f))
    assert result.content == ""
    assert "Binary content detected" in result.error


def test_read_file_preserves_unicode_and_replacement_behavior(executor, tmp_path):
    f = tmp_path / "text.txt"
    f.write_bytes("中文\n".encode("utf-8") + b"legacy\xff\n")
    result = executor.read_file(str(f))
    assert result.error is None
    assert result.content == "中文\nlegacy\ufffd\n"
    assert result.total_lines == 2


# ---------------------------------------------------------------------------
# write_file
# ---------------------------------------------------------------------------

def test_write_file_creates_file(executor, tmp_path):
    """write_file creates file with given content and returns success."""
    target = tmp_path / "out.txt"
    result = executor.write_file(str(target), "hello world")
    assert isinstance(result, WriteResult)
    assert result.success is True
    assert result.bytes_written > 0
    # Verify actual file content
    assert target.read_text(encoding="utf-8") == "hello world"


def test_write_file_creates_parent_dirs(executor, tmp_path):
    """write_file creates missing parent directories."""
    target = tmp_path / "sub" / "deep" / "file.txt"
    result = executor.write_file(str(target), "nested")
    assert result.success is True
    assert target.read_text(encoding="utf-8") == "nested"


def test_write_file_overwrites_existing(executor, tmp_path):
    """write_file overwrites existing file content."""
    f = tmp_path / "existing.txt"
    f.write_text("old content", encoding="utf-8")
    executor.write_file(str(f), "new content")
    assert f.read_text(encoding="utf-8") == "new content"


# ---------------------------------------------------------------------------
# edit_file
# ---------------------------------------------------------------------------

def test_edit_file_precise_replace(executor, tmp_path):
    """edit_file replaces old_str with new_str exactly."""
    f = tmp_path / "edit.txt"
    f.write_text("Hello World\nFoo Bar\n", encoding="utf-8")

    result = executor.edit_file(str(f), "World", "Universe")
    assert isinstance(result, ExecResult)
    assert result.exit_code == 0
    assert f.read_text(encoding="utf-8") == "Hello Universe\nFoo Bar\n"


def test_edit_file_old_str_not_found(executor, tmp_path):
    """edit_file returns error when old_str is not in file."""
    f = tmp_path / "edit2.txt"
    f.write_text("nothing to change\n", encoding="utf-8")

    result = executor.edit_file(str(f), "MISSING", "replacement")
    assert result.error is not None
    assert "not found" in result.error.lower()
    # File should be unchanged
    assert f.read_text(encoding="utf-8") == "nothing to change\n"


def test_edit_file_replace_all(executor, tmp_path):
    """replace_all=True replaces every occurrence."""
    f = tmp_path / "multi.txt"
    f.write_text("a a a\n", encoding="utf-8")

    result = executor.edit_file(str(f), "a", "b", replace_all=True)
    assert result.exit_code == 0
    assert f.read_text(encoding="utf-8") == "b b b\n"


def test_edit_file_non_unique_without_replace_all_errors(executor, tmp_path):
    """old_str appearing multiple times without replace_all returns error."""
    f = tmp_path / "dup.txt"
    f.write_text("x y x y x\n", encoding="utf-8")

    result = executor.edit_file(str(f), "x", "z")
    assert result.error is not None
    assert "not unique" in result.error.lower() or "multiple" in result.error.lower()


# ---------------------------------------------------------------------------
# list_dir
# ---------------------------------------------------------------------------

def test_list_dir_returns_entries(executor, tmp_path):
    """list_dir returns list of dicts with name/path/is_dir for each entry."""
    (tmp_path / "file_a.txt").write_text("a", encoding="utf-8")
    (tmp_path / "subdir").mkdir()

    entries = executor.list_dir(str(tmp_path))
    assert isinstance(entries, list)
    names = [e["name"] for e in entries]
    assert "file_a.txt" in names
    assert "subdir" in names

    # Check is_dir flag
    for e in entries:
        if e["name"] == "subdir":
            assert e["is_dir"] is True
        if e["name"] == "file_a.txt":
            assert e["is_dir"] is False


def test_list_dir_nonexistent_returns_empty_or_error(executor, tmp_path):
    """list_dir on nonexistent path returns empty list or error entries."""
    entries = executor.list_dir(str(tmp_path / "nope"))
    # Should not crash — return empty list
    assert isinstance(entries, list)


# ---------------------------------------------------------------------------
# fetch_url
# ---------------------------------------------------------------------------

def test_fetch_url_returns_content(executor):
    """fetch_url returns FileContent with fetched text."""
    mock_resp = MagicMock()
    mock_resp.__enter__.return_value = mock_resp
    mock_resp.read.return_value = b"<html><body>Hello</body></html>"
    mock_resp.headers.get.return_value = "text/html"

    with patch(
        "agentflow_runtime.executors.host._SAFE_URL_OPENER.open",
        return_value=mock_resp,
    ):
        result = executor.fetch_url("https://example.com/test")
    assert isinstance(result, FileContent)
    assert result.error is None
    assert "Hello" in result.content


def test_fetch_url_decompresses_gzip_content(executor):
    """fetch_url decodes gzip transport bytes before charset/HTML handling."""
    import gzip

    mock_resp = MagicMock()
    mock_resp.__enter__.return_value = mock_resp
    mock_resp.read.return_value = gzip.compress(b"<html><body>Hello gzip</body></html>")
    mock_resp.headers.get.side_effect = lambda name, default="": {
        "Content-Encoding": "gzip",
        "Content-Type": "text/html; charset=utf-8",
    }.get(name, default)

    with patch(
        "agentflow_runtime.executors.host._SAFE_URL_OPENER.open",
        return_value=mock_resp,
    ):
        result = executor.fetch_url("https://example.com/test")

    assert result.error is None
    assert "Hello gzip" in result.content


def test_fetch_url_connection_error_returns_error(executor):
    """fetch_url on unreachable URL returns FileContent with error."""
    import urllib.error

    with patch(
        "agentflow_runtime.executors.host._SAFE_URL_OPENER.open",
        side_effect=urllib.error.URLError("no connection"),
    ):
        result = executor.fetch_url("https://unreachable.invalid/page")
    assert result.error is not None
