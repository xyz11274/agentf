"""E2BExecutor behavior tests — verifies e2b SDK direct-connect sandbox execution.

All tests mock e2b_code_interpreter.Sandbox — no real sandbox or network required.
Reference: ADR 20260724-tools-runtime-refactor.md Phase 3 (e2b provider).
"""

import os
from unittest.mock import MagicMock, patch

import pytest

from agentflow_runtime.executors.base import ExecResult, FileContent
from agentflow_runtime.executors.e2b import E2BExecutor


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

VALID_ENDPOINT = "https://218.245.96.148:443/sandbox/a1b2c3d4-e5f6-7890-abcd-ef1234567890/49983"
SANDBOX_ID = "a1b2c3d4-e5f6-7890-abcd-ef1234567890"
HOST_PKG_DIR = "/fake/agent/package"


@pytest.fixture
def mock_sandbox():
    """A mock e2b Sandbox instance with commands.run returning empty success."""
    sb = MagicMock()
    sb.sandbox_id = SANDBOX_ID
    sb._envd_api_url = f"https://218.245.96.148:443/sandbox/{SANDBOX_ID}/49983"
    execution = MagicMock()
    execution.stdout = ""
    execution.stderr = ""
    execution.exit_code = 0
    sb.commands.run.return_value = execution
    return sb


@pytest.fixture
def mock_e2b_module(mock_sandbox):
    """Inject a fake e2b_code_interpreter module into sys.modules."""
    mock_mod = MagicMock()
    mock_mod.Sandbox.connect.return_value = mock_sandbox
    with patch.dict("sys.modules", {"e2b_code_interpreter": mock_mod}):
        yield mock_mod


@pytest.fixture
def executor(monkeypatch):
    """Create an E2BExecutor with required env vars set."""
    monkeypatch.setenv("E2B_API_KEY", "test-key-12345")
    monkeypatch.setenv("AF_AGENT_PACKAGE_DIR", HOST_PKG_DIR)
    return E2BExecutor(endpoint=VALID_ENDPOINT)


@pytest.fixture
def executor_with_mocks(executor, mock_e2b_module, mock_sandbox):
    """Executor + mock SDK injected — ready for exec_command calls."""
    # Stub _download_images to avoid real file I/O during exec tests
    executor._download_images = MagicMock()
    return executor, mock_sandbox


# ---------------------------------------------------------------------------
# __init__
# ---------------------------------------------------------------------------

def test_init_extracts_sandbox_id(monkeypatch):
    """endpoint containing /sandbox/{uuid}/ correctly extracts sandbox_id."""
    monkeypatch.setenv("E2B_API_KEY", "test-key")
    exe = E2BExecutor(endpoint=VALID_ENDPOINT)
    assert exe.sandbox_id == SANDBOX_ID


def test_init_invalid_endpoint_raises(monkeypatch):
    """endpoint without /sandbox/{id}/ pattern raises ValueError."""
    monkeypatch.setenv("E2B_API_KEY", "test-key")
    with pytest.raises(ValueError, match="Cannot parse sandbox_id"):
        E2BExecutor(endpoint="https://example.com/no/sandbox/here")


def test_init_no_api_key_raises(monkeypatch):
    """Missing E2B_API_KEY raises RuntimeError."""
    monkeypatch.delenv("E2B_API_KEY", raising=False)
    with pytest.raises(RuntimeError, match="E2B_API_KEY not set"):
        E2BExecutor(endpoint=VALID_ENDPOINT)


# ---------------------------------------------------------------------------
# exec_command
# ---------------------------------------------------------------------------

def test_exec_command_calls_sdk_run(executor_with_mocks):
    """exec_command calls Sandbox.connect + sb.commands.run.

    _connect 现在会先调一次 commands.run 做 mkdir -p SANDBOX_WORKSPACE（幂等保 cwd
    不崩），真命令是最后一次调用——检查最后一次即可，不再用 assert_called_once。
    """
    exe, sb = executor_with_mocks
    exe.exec_command("echo hello", timeout=10)

    assert sb.commands.run.call_count >= 1
    args, kwargs = sb.commands.run.call_args
    # command is positional arg
    assert "echo hello" in args[0]
    assert kwargs["cwd"] == E2BExecutor.SANDBOX_WORKSPACE


def test_exec_command_forwards_workdir_override(executor_with_mocks):
    """exec_command 透传显式 workdir 给 SDK cwd——webshell 跨命令 cwd 延续。

    bash.py 缺省 cwd 透传宿主 agent_package_dir，executor 经 _to_sandbox_path 翻译；
    外部显式 cwd override（如 webshell 延续）必须透传，恢复 cwd override 功能。
    用宿主路径 fixture（生产真实输入）验证 _to_sandbox_path 翻译后 cwd 是沙箱路径。
    """
    exe, sb = executor_with_mocks
    # 宿主子目录（生产中 webshell 延续的真实形态），应被 _to_sandbox_path 翻译
    host_subdir = os.path.join(HOST_PKG_DIR, "outputs")
    exe.exec_command("pwd", workdir=host_subdir)

    _, kwargs = sb.commands.run.call_args
    assert kwargs["cwd"] == os.path.join(E2BExecutor.SANDBOX_WORKSPACE, "outputs")


def test_exec_command_translates_paths(executor_with_mocks):
    """agent_package_dir in command is replaced with /home/user/workspace."""
    exe, sb = executor_with_mocks
    cmd = f"python3 {HOST_PKG_DIR}/scripts/run.py --input {HOST_PKG_DIR}/data/test.json"
    exe.exec_command(cmd)

    args, _ = sb.commands.run.call_args
    sent_cmd = args[0]
    assert HOST_PKG_DIR not in sent_cmd
    assert E2BExecutor.SANDBOX_WORKSPACE in sent_cmd


def test_exec_command_injects_env_source_prefix(executor_with_mocks):
    """命令前缀含 `set -a && source .env && set +a` — 让 .env 的 KEY=VAL 进子进程 os.environ。

    覆盖设计文档 sandbox-env-injection.md §6 敲定方案:
    - .env 标准写法无 export,source 后只是 shell 变量,子进程读不到
    - set -a 让 source 期间赋值自动 export,子进程能读
    - 宿主机 os.environ 不透传(防 PATH/HOME 污染沙箱)
    """
    exe, sb = executor_with_mocks
    exe.exec_command("echo hi")

    args, _ = sb.commands.run.call_args
    sent_cmd = args[0]
    # 必须含 set -a + source .env + set +a 前缀
    assert "set -a" in sent_cmd
    assert "source /home/user/workspace/.env" in sent_cmd
    assert "set +a" in sent_cmd
    # 命令本体在前缀之后
    assert sent_cmd.endswith("echo hi") or "echo hi" in sent_cmd
    # 宿主机 os.environ 不应被透传——不应出现裸 export PATH/HOME
    assert "export PATH=" not in sent_cmd
    assert "export HOME=" not in sent_cmd


def test_exec_command_env_prefix_has_file_check(executor_with_mocks):
    """.env 不存在时前缀含 `[ -f ... ] &&` 兜底,source 不崩,沙箱用原生环境。

    锁住不变量:源前缀无条件拼,但 [ -f ] 兜底让 .env 缺失时不 source(防 shell 报错)。
    """
    exe, sb = executor_with_mocks
    exe.exec_command("echo hi")

    args, _ = sb.commands.run.call_args
    sent_cmd = args[0]
    # 必须含 [ -f ... ] && 兜底
    assert "[ -f /home/user/workspace/.env ]" in sent_cmd
    # 兜底 + set -a + source + set +a 链完整
    assert "[ -f" in sent_cmd and "set -a" in sent_cmd and "source" in sent_cmd


def test_exec_command_returns_exec_result(executor_with_mocks):
    """Mock SDK returns stdout/exit_code → ExecResult mapped correctly."""
    exe, sb = executor_with_mocks
    sb.commands.run.return_value.stdout = "output line\n"
    sb.commands.run.return_value.stderr = ""
    sb.commands.run.return_value.exit_code = 0

    result = exe.exec_command("echo output")

    assert isinstance(result, ExecResult)
    assert result.stdout == "output line\n"
    assert result.exit_code == 0
    assert result.error is None


def test_exec_command_translates_output_paths(executor_with_mocks):
    """sandbox paths in stdout/stderr are translated back to host paths."""
    exe, sb = executor_with_mocks
    sb.commands.run.return_value.stdout = f"saved to {E2BExecutor.SANDBOX_WORKSPACE}/outputs/result.txt"
    sb.commands.run.return_value.exit_code = 0

    result = exe.exec_command("save file")

    assert HOST_PKG_DIR in result.stdout
    assert E2BExecutor.SANDBOX_WORKSPACE not in result.stdout


def test_exec_command_sdk_exception_extracts_results(executor_with_mocks):
    """SDK exception with stdout/exit_code attributes is extracted gracefully."""
    exe, sb = executor_with_mocks

    # Simulate SDK raising exception with result attributes
    exc = Exception("Command exited with code 1")
    exc.stdout = f"partial output {E2BExecutor.SANDBOX_WORKSPACE}/log.txt"
    exc.stderr = "error msg"
    exc.exit_code = 1
    sb.commands.run.side_effect = exc

    result = exe.exec_command("failing cmd")

    assert isinstance(result, ExecResult)
    assert result.exit_code == 1
    assert "partial output" in result.stdout
    assert HOST_PKG_DIR in result.stdout  # translated
    assert result.error is not None
    assert "Sandbox SDK exec failed" in result.error


# ---------------------------------------------------------------------------
# read_file
# ---------------------------------------------------------------------------

def test_e2b_read_file_uses_sandbox_sdk(executor_with_mocks):
    """E2BExecutor.read_file must use Sandbox commands.run, not host open()."""
    exe, sb = executor_with_mocks
    sb.commands.run.return_value.stdout = "sandbox content\n"
    sb.commands.run.return_value.stderr = ""
    sb.commands.run.return_value.exit_code = 0

    result = exe.read_file(f"{E2BExecutor.SANDBOX_WORKSPACE}/test.txt")

    # _connect 先调一次 mkdir，read_file 的 cat 是最后一次调用——不再 assert_called_once
    assert sb.commands.run.call_count >= 1
    args, _ = sb.commands.run.call_args
    sent_cmd = args[0]
    assert "cat" in sent_cmd, f"Expected cat command but got: {sent_cmd}"
    assert "test.txt" in sent_cmd

    assert isinstance(result, FileContent)
    assert result.error is None
    assert result.content == "sandbox content\n"


def test_e2b_read_file_returns_content_with_sentinel(executor_with_mocks):
    """read_file parses the EXIT_CODE sentinel and returns file content."""
    exe, sb = executor_with_mocks
    sb.commands.run.return_value.stdout = '{"key": "value"}\n\nEXIT_CODE=0\n'
    sb.commands.run.return_value.stderr = ""
    sb.commands.run.return_value.exit_code = 0

    result = exe.read_file(f"{E2BExecutor.SANDBOX_WORKSPACE}/config.json")

    assert isinstance(result, FileContent)
    assert result.error is None
    assert "value" in result.content
    assert result.total_lines == 2


def test_e2b_read_file_sandbox_file_not_found(executor_with_mocks):
    """read_file returns error when sandbox cat fails (non-zero sentinel)."""
    exe, sb = executor_with_mocks
    # Simulate file-not-found: cat to stderr, sentinel exit_code=1
    sb.commands.run.return_value.stdout = "\nEXIT_CODE=1\n"
    sb.commands.run.return_value.stderr = "cat: nonexistent.txt: No such file or directory"
    sb.commands.run.return_value.exit_code = 0

    result = exe.read_file(f"{E2BExecutor.SANDBOX_WORKSPACE}/nonexistent.txt")

    assert isinstance(result, FileContent)
    assert result.error is not None
    assert "not found" in result.error.lower() or "not_found" in result.error.lower()


# ---------------------------------------------------------------------------
# sandbox_patch.apply_ssl_patch
# ---------------------------------------------------------------------------

def test_apply_ssl_patch_is_idempotent():
    """Calling apply_ssl_patch twice does not stack patches."""
    from agentflow_runtime.common.sandbox_patch import apply_ssl_patch

    import ssl

    apply_ssl_patch()
    ctx1 = ssl.create_default_context()
    first_check = ctx1.check_hostname

    apply_ssl_patch()  # second call — should be no-op
    ctx2 = ssl.create_default_context()

    assert first_check is False
    assert ctx2.check_hostname is False
    assert ctx2.verify_mode == ssl.CERT_NONE
