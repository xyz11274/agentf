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
    """Generic E2B commands never source the agent package .env."""
    exe, sb = executor_with_mocks
    exe.exec_command("echo hi")

    args, _ = sb.commands.run.call_args
    sent_cmd = args[0]
    assert sent_cmd == "echo hi"
    assert ".env" not in sent_cmd
    assert "source " not in sent_cmd
    assert "set -a" not in sent_cmd


def test_exec_command_env_prefix_has_file_check(executor_with_mocks):
    """The retained compatibility helper returns an empty prefix."""
    exe, sb = executor_with_mocks
    assert exe._build_env_source_prefix() == ""

    exe.exec_command("echo hi")
    args, _ = sb.commands.run.call_args
    assert args[0] == "echo hi"


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
    # F7.1 诊断契约:错误信息必须携带 "Sandbox SDK exec failed" 前缀与命令上下文,
    # 让 LLM 从空 error 中能自诊断路径错(2026-08-14 review 恢复误删断言)。
    assert "Sandbox SDK exec failed" in result.error
    assert "cmd=" in result.error


def test_download_images_pulls_video_from_sandbox(executor, monkeypatch, tmp_path):
    """bash 成功后 stdout 中的视频路径也应被拉回宿主机(与图片同一机制)。

    回归 bug(2026-08-14 实测):视频生成成功但宿主机无文件,collect 阶段
    os.path.isfile 失败 → skipped_missing → 前端收不到视频。根因:
    _IMAGE_PATH_RE 只匹配 png/jpeg/webp/gif,bash 执行后视频未从沙箱拉回。
    """
    exe = executor
    exe.agent_package_dir = str(tmp_path)
    sb = MagicMock()
    video_bytes = b"\x00\x00\x00\x18ftypmp42"  # 假 mp4 头
    monkeypatch.setattr(
        exe,
        "_fetch_sandbox_file",
        lambda s, path: video_bytes if path.endswith(".mp4") else None,
    )

    stdout = f"saved video to {E2BExecutor.SANDBOX_WORKSPACE}/outfit.mp4"
    exe._download_images(sb, stdout)

    target = tmp_path / "outfit.mp4"
    assert target.exists(), "视频应从沙箱拉回宿主机"
    assert target.read_bytes() == video_bytes


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

def test_apply_ssl_patch_is_idempotent(monkeypatch):
    """The envd patch is idempotent and leaves global TLS strict."""
    from agentflow_runtime.common.sandbox_patch import apply_ssl_patch

    import ssl

    monkeypatch.delenv("E2B_API_URL", raising=False)
    original_factory = ssl.create_default_context
    apply_ssl_patch()
    apply_ssl_patch()
    context = ssl.create_default_context()

    assert ssl.create_default_context is original_factory
    assert context.check_hostname is True
    assert context.verify_mode == ssl.CERT_REQUIRED


# ---------------------------------------------------------------------------
# write_file — 沙箱内写入（不写回宿主机）
# ---------------------------------------------------------------------------

def test_e2b_write_file_writes_inside_sandbox(executor_with_mocks, tmp_path, monkeypatch):
    """E2BExecutor.write_file must write via e2b SDK into the sandbox,
    NOT to the host agent_package_dir (跨会话隔离，避免宿主机残留)."""
    exe, sb = executor_with_mocks
    # sb.files 是 MagicMock 自动生成 —— 显式配置 write 返回，避免 mock 自动返回值
    sb.files.write.return_value = None

    sandbox_path = f"{E2BExecutor.SANDBOX_WORKSPACE}/harness/skills/outfit-stylist/outfit-2026-08-10.html"
    result = exe.write_file(sandbox_path, "<html>lookbook</html>")

    # 1) 写入走沙箱 SDK，而不是宿主机 open()
    sb.files.write.assert_called_once()
    call_path, call_data = sb.files.write.call_args[0]
    assert call_path == sandbox_path
    assert isinstance(call_data, bytes)
    assert b"<html>lookbook</html>" in call_data

    # 2) 宿主机 agent_package_dir 下不得产生文件
    assert not (tmp_path / "harness" / "skills" / "outfit-stylist" / "outfit-2026-08-10.html").exists()

    # 3) 返回体携带沙箱内 path（供产物收集 extract_structured_artifacts 识别）
    assert result.success is True
    assert result.path == sandbox_path
    assert result.bytes_written == len("<html>lookbook</html>".encode("utf-8"))


# ---------------------------------------------------------------------------
# list_dir — is_dir 推断走 FileType enum identity（非字符串 introspection）
# ---------------------------------------------------------------------------

def test_e2b_list_dir_is_dir_uses_filetype_identity(executor_with_mocks):
    """list_dir 的 is_dir 必须用 FileType enum identity 判断，禁止凭 str(Enum) 推断。

    回归 bug：原代码 `str(FileType.DIR).upper().startswith("DIR")` 永远 False
    （str(FileType.DIR)=="FileType.DIR"，开头是 "FILETYPE" 非 "DIR"），
    导致所有目录被错误标记为 is_dir=False。2026-08-10 review 发现。

    本测用真 EntryInfo dataclass 实例（非 MagicMock 自动生成），贴近 SDK
    真实返回形态，避免 mock 掩盖 enum identity 推断错误。
    """
    from datetime import datetime, timezone
    from e2b_code_interpreter import FileType
    from e2b.sandbox.filesystem.filesystem import EntryInfo

    exe, sb = executor_with_mocks
    sandbox_dir = f"{E2BExecutor.SANDBOX_WORKSPACE}/harness/skills/outfit-stylist"
    # 真 EntryInfo 实例：一个目录 + 一个文件，type 用真 FileType enum
    sb.files.list.return_value = [
        EntryInfo(
            name="outfit-stylist",
            type=FileType.DIR,
            path=f"{sandbox_dir}/outfit-stylist",
            metadata=None,
            size=0, mode=0, permissions="rwxr-xr-x",
            owner="user", group="user",
            modified_time=datetime(2026, 8, 10, tzinfo=timezone.utc),
            symlink_target=None,
        ),
        EntryInfo(
            name="outfit-2026-08-10.html",
            type=FileType.FILE,
            path=f"{sandbox_dir}/outfit-2026-08-10.html",
            metadata=None,
            size=1024, mode=0, permissions="rw-r--r--",
            owner="user", group="user",
            modified_time=datetime(2026, 8, 10, tzinfo=timezone.utc),
            symlink_target=None,
        ),
    ]

    result = exe.list_dir(sandbox_dir, recursive=False)

    # 不变量：DIR → is_dir=True，FILE → is_dir=False（enum identity 判断）
    assert len(result) == 2
    assert result[0]["name"] == "outfit-stylist"
    assert result[0]["is_dir"] is True, "FileType.DIR 必须被识别为目录"
    assert result[1]["name"] == "outfit-2026-08-10.html"
    assert result[1]["is_dir"] is False, "FileType.FILE 必须被识别为文件"


# ---------------------------------------------------------------------------
# _to_sandbox_path — agent_package_dir 前缀边界（姊妹目录不误替换）
# ---------------------------------------------------------------------------

def test_e2b_to_sandbox_path_rejects_sibling_prefix(executor):
    """_to_sandbox_path 必须用 os.sep 边界判断 agent_package_dir 前缀，
    避免 /fake/agent/package-backup/... 姊妹目录被误替换为沙箱路径。

    回归 bug：原 `path.startswith(agent_package_dir)` + `replace(...)` 无边界，
    把 /fake/agent/package-backup/foo.html 错转为 /home/user/workspace-backup/foo.html。
    2026-08-10 review 发现。
    """
    # agent_package_dir = /fake/agent/package（fixture 设的 HOST_PKG_DIR）
    sibling = f"{HOST_PKG_DIR}-backup/foo.html"
    result = executor._to_sandbox_path(sibling)
    assert result == sibling, f"姊妹目录前缀碰撞应原样返回，实际: {result}"

    # 正向：包内路径 → 沙箱路径
    inside = f"{HOST_PKG_DIR}/harness/skills/outfit.html"
    result_inside = executor._to_sandbox_path(inside)
    assert result_inside == f"{E2BExecutor.SANDBOX_WORKSPACE}/harness/skills/outfit.html"

    # 边界：agent_package_dir 本身 → 沙箱 workspace 根
    result_root = executor._to_sandbox_path(HOST_PKG_DIR)
    assert result_root == E2BExecutor.SANDBOX_WORKSPACE


def test_fetch_url_uses_hardened_host_transport(executor, monkeypatch):
    expected = FileContent(content="safe")
    fetch = MagicMock(return_value=expected)
    monkeypatch.setattr(
        "agentflow_runtime.executors.host.HostExecutor.fetch_url", fetch,
    )

    result = executor.fetch_url("https://public.example/page", timeout=17)

    assert result is expected
    fetch.assert_called_once_with("https://public.example/page", 17)
