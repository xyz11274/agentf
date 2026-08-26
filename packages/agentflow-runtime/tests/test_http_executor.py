"""HTTPExecutor behavior tests — verifies HTTP POST to sandbox_http_server.

All tests mock requests.post — no real network or sandbox required.
Reference: ADR 20260724-tools-runtime-refactor.md Phase 2.
"""

import json
from unittest.mock import MagicMock, patch

import pytest
import requests

from agentflow_runtime.executors.base import ExecResult, FileContent, WriteResult
from agentflow_runtime.executors.http import HTTPExecutor


# Module path for patching requests inside http.py
_POST = "agentflow_runtime.executors.http.requests.post"

ENDPOINT = "http://localhost:9999"


@pytest.fixture
def executor():
    return HTTPExecutor(endpoint=ENDPOINT)


def _mock_resp(data: dict, status_code: int = 200):
    """Build a mock response object whose .json() returns *data*."""
    m = MagicMock()
    m.json.return_value = data
    m.status_code = status_code
    return m


# ---------------------------------------------------------------------------
# exec_command
# ---------------------------------------------------------------------------

def test_exec_command_posts_correct_payload(executor):
    """exec_command sends POST /exec with tool=bash, tool_params.command, opts.timeout_ms."""
    resp = _mock_resp({"stdout": "", "exit_code": 0})
    with patch(_POST, return_value=resp) as mock_post:
        executor.exec_command("echo hi", timeout=10)

    mock_post.assert_called_once()
    _, kwargs = mock_post.call_args
    payload = kwargs["json"]
    assert payload["tool"] == "bash"
    assert payload["tool_params"]["command"] == "echo hi"
    assert payload["opts"]["timeout_ms"] == 10 * 1000


def test_exec_command_returns_exec_result(executor):
    """exec_command maps response JSON to ExecResult fields."""
    resp = _mock_resp({"stdout": "hello\n", "stderr": "", "exit_code": 0})
    with patch(_POST, return_value=resp):
        result = executor.exec_command("echo hello")

    assert isinstance(result, ExecResult)
    assert result.stdout == "hello\n"
    assert result.exit_code == 0
    assert result.error is None


def test_exec_command_connection_error_returns_error(executor):
    """ConnectionError produces ExecResult.error containing endpoint, no crash."""
    with patch(_POST, side_effect=requests.exceptions.ConnectionError("refused")):
        result = executor.exec_command("echo hi")

    assert isinstance(result, ExecResult)
    assert result.error is not None
    assert ENDPOINT in result.error


def test_exec_command_invalid_json_returns_error(executor):
    """Non-JSON response produces ExecResult.error, no crash."""
    resp = MagicMock()
    resp.json.side_effect = json.JSONDecodeError("expecting value", "doc", 0)
    with patch(_POST, return_value=resp):
        result = executor.exec_command("echo hi")

    assert isinstance(result, ExecResult)
    assert result.error is not None


def test_http_503_propagates_error_to_read_file(executor):
    """HTTP overload must not look like a successful empty file read."""
    resp = _mock_resp(
        {
            "stdout": "",
            "stderr": "Concurrent tool execution limit reached (8). Retry later.",
            "exit_code": 1,
        },
        status_code=503,
    )
    with patch(_POST, return_value=resp):
        result = executor.read_file("/some/path.txt")

    assert result.content == ""
    assert result.error is not None
    assert "HTTP 503" in result.error
    assert "Concurrent tool execution limit reached" in result.error


def test_nonzero_exit_propagates_error_to_write_file(executor):
    """A JSON ExecResult failure must retain its stderr as WriteResult.error."""
    resp = _mock_resp(
        {"stdout": "", "stderr": "permission denied", "exit_code": 1}
    )
    with patch(_POST, return_value=resp):
        result = executor.write_file("/out.txt", "content")

    assert result.success is False
    assert result.error is not None
    assert "permission denied" in result.error


# ---------------------------------------------------------------------------
# read_file
# ---------------------------------------------------------------------------

def test_read_file_posts_correct_tool(executor):
    """read_file sends tool=read_file with tool_params.path."""
    resp = _mock_resp({"stdout": "file body", "exit_code": 0})
    with patch(_POST, return_value=resp) as mock_post:
        result = executor.read_file("/some/path.txt")

    mock_post.assert_called_once()
    _, kwargs = mock_post.call_args
    payload = kwargs["json"]
    assert payload["tool"] == "read_file"
    assert payload["tool_params"]["path"] == "/some/path.txt"
    assert isinstance(result, FileContent)
    assert result.content == "file body"


# ---------------------------------------------------------------------------
# write_file
# ---------------------------------------------------------------------------

def test_write_file_posts_correct_tool(executor):
    """write_file sends tool=write_file with path + content in tool_params."""
    resp = _mock_resp({"stdout": "11", "exit_code": 0})
    with patch(_POST, return_value=resp) as mock_post:
        result = executor.write_file("/out.txt", "hello world")

    mock_post.assert_called_once()
    _, kwargs = mock_post.call_args
    payload = kwargs["json"]
    assert payload["tool"] == "write_file"
    assert payload["tool_params"]["path"] == "/out.txt"
    assert payload["tool_params"]["content"] == "hello world"
    assert isinstance(result, WriteResult)
    assert result.success is True


# ---------------------------------------------------------------------------
# edit_file
# ---------------------------------------------------------------------------

def test_edit_file_posts_correct_tool(executor):
    """edit_file sends tool=edit_file with path/old_str/new_str/replace_all."""
    resp = _mock_resp({"stdout": "Edited", "exit_code": 0})
    with patch(_POST, return_value=resp) as mock_post:
        result = executor.edit_file("/f.txt", "old", "new", replace_all=True)

    mock_post.assert_called_once()
    _, kwargs = mock_post.call_args
    payload = kwargs["json"]
    assert payload["tool"] == "edit_file"
    tp = payload["tool_params"]
    assert tp["path"] == "/f.txt"
    assert tp["old_str"] == "old"
    assert tp["new_str"] == "new"
    assert tp["replace_all"] is True
    assert isinstance(result, ExecResult)
    assert result.exit_code == 0


# ---------------------------------------------------------------------------
# list_dir
# ---------------------------------------------------------------------------

def test_list_dir_posts_correct_tool(executor):
    """list_dir sends tool=list_dir and returns parsed entries from stdout."""
    entries = [{"name": "a.txt", "path": "/d/a.txt", "is_dir": False}]
    resp = _mock_resp({"stdout": json.dumps(entries), "exit_code": 0})
    with patch(_POST, return_value=resp) as mock_post:
        result = executor.list_dir("/some/dir")

    mock_post.assert_called_once()
    _, kwargs = mock_post.call_args
    assert kwargs["json"]["tool"] == "list_dir"
    assert isinstance(result, list)
    assert len(result) == 1
    assert result[0]["name"] == "a.txt"


# ---------------------------------------------------------------------------
# fetch_url
# ---------------------------------------------------------------------------

def test_fetch_url_posts_correct_tool(executor):
    """fetch_url sends tool=web_fetch with url in tool_params."""
    resp = _mock_resp({"stdout": "<html>page</html>", "exit_code": 0})
    with patch(_POST, return_value=resp) as mock_post:
        result = executor.fetch_url("https://example.com/page")

    mock_post.assert_called_once()
    _, kwargs = mock_post.call_args
    payload = kwargs["json"]
    assert payload["tool"] == "web_fetch"
    assert payload["tool_params"]["url"] == "https://example.com/page"
    assert isinstance(result, FileContent)
    assert "page" in result.content


# ---------------------------------------------------------------------------
# opts propagation
# ---------------------------------------------------------------------------

def test_timeout_passed_correctly(executor):
    """timeout (seconds) is converted to timeout_ms in opts."""
    resp = _mock_resp({"stdout": "", "exit_code": 0})
    with patch(_POST, return_value=resp) as mock_post:
        executor.exec_command("ls", timeout=45)

    _, kwargs = mock_post.call_args
    assert kwargs["json"]["opts"]["timeout_ms"] == 45000


def test_workdir_passed_in_opts(executor):
    """workdir is forwarded as opts.working_dir."""
    resp = _mock_resp({"stdout": "", "exit_code": 0})
    with patch(_POST, return_value=resp) as mock_post:
        executor.exec_command("pwd", workdir="/custom/work")

    _, kwargs = mock_post.call_args
    assert kwargs["json"]["opts"]["working_dir"] == "/custom/work"
