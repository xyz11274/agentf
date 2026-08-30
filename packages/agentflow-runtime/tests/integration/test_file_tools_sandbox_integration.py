"""T046 read_file/write_file/edit_file 改造后集成测试——AC-US2-001/002/003 全链路。

覆盖 AC-US2-001（read_file 返回内容）/ AC-US2-002（../etc/passwd 拒绝含路径名）/
AC-US2-003（write_file 落 workdir），引用 TC-SBOX-004/023/061。

运行方式：pytest packages/agentflow-runtime/tests/integration/test_file_tools_sandbox_integration.py -v
"""
import json
import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[4]
_SERVER = str(_REPO_ROOT / "docker" / "sandbox" / "http_server" / "sandbox_http_server.py")
_TOOLS = str(_REPO_ROOT / "packages" / "agentflow-runtime" / "src" / "agentflow_runtime" / "harness" / "tools")
sys.path.insert(0, _TOOLS)
from read_file import execute as read_execute  # noqa: E402
from write_file import execute as write_execute  # noqa: E402
from edit_file import execute as edit_execute  # noqa: E402


def _find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture
def sandbox_server(monkeypatch: pytest.MonkeyPatch):
    """拉起 sandbox_http_server.py，yield (endpoint, workdir)，销毁子进程。

    chdir(workdir)：默认无 AF_SANDBOX_MODE 时工具走 HostExecutor，
    用 os.getcwd() 决定落点——不 chdir 会把测试文件写进 pytest cwd
    （包根目录）造成残留，且 workdir teardown 清理不到。
    """
    port = _find_free_port()
    workdir = Path(__file__).resolve().parent / "_sandbox_workdir_test"
    workdir.mkdir(exist_ok=True)
    monkeypatch.chdir(workdir)
    proc = subprocess.Popen(
        [sys.executable, _SERVER, "--port", str(port), "--host", "127.0.0.1"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        cwd=str(workdir),
    )
    endpoint = f"http://127.0.0.1:{port}"
    import requests
    for _ in range(50):
        try:
            requests.post(
                f"{endpoint}/exec",
                json={"tool": "bash", "tool_params": {"command": "echo ready"}, "opts": {}},
                timeout=1,
            )
            break
        except Exception:
            if proc.poll() is not None:
                pytest.fail(f"server exited: {proc.stderr.read().decode()}")
            time.sleep(0.1)
    else:
        proc.kill()
        pytest.fail("server startup timeout")
    yield endpoint, str(workdir)
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=2)
    # 清理 workdir 测试文件（保留目录供下次测试）
    for f in workdir.glob("*.txt"):
        try:
            f.unlink()
        except OSError:
            pass


def test_server_does_not_reenter_inherited_endpoint(tmp_path):
    """HTTP server 作为执行链终点时，不得把 runtime tool 再路由回自身。

    复现 daemon 启动环境：子进程继承 ``AF_SANDBOX_MODE=local`` 及自身
    endpoint。修复前一次 bash 调用会递归占满 8 个请求槽，最终返回
    ``Concurrent tool execution limit reached (8)``。
    """
    port = _find_free_port()
    endpoint = f"http://127.0.0.1:{port}"
    child_env = os.environ.copy()
    child_env["AF_SANDBOX_MODE"] = "local"
    child_env["AF_SANDBOX_ENDPOINT"] = endpoint
    child_env["AF_SANDBOX_WORKSPACE"] = str(tmp_path)
    child_env["AF_ALLOW_UNSAFE_LOCAL_SECRET_TOOLS"] = "1"

    proc = subprocess.Popen(
        [sys.executable, _SERVER, "--port", str(port), "--host", "127.0.0.1"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=str(tmp_path),
        env=child_env,
    )
    try:
        import requests

        response = None
        for _ in range(50):
            try:
                response = requests.post(
                    f"{endpoint}/exec",
                    json={
                        "tool": "bash",
                        "tool_params": {"command": "printf terminal-ok"},
                        "opts": {},
                    },
                    timeout=3,
                )
                break
            except requests.ConnectionError:
                if proc.poll() is not None:
                    pytest.fail(f"server exited: {proc.stderr.read().decode()}")
                time.sleep(0.1)
        else:
            pytest.fail("server startup timeout")

        assert response is not None
        result = response.json()
        assert response.status_code == 200
        assert result.get("exit_code") == 0, result
        assert result.get("stdout") == "terminal-ok", result
        assert "Concurrent tool execution limit" not in result.get("stderr", "")
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=2)


# AC-US2-003: write_file 写 output.txt 落沙箱内 workdir，宿主机对应 workdir 下可见
def test_write_file_lands_in_workdir(sandbox_server):
    """AC-US2-003 + TC-SBOX-004: write_file 落 workdir。

    业务规则: B4 文件落沙箱内 workdir
    错误模型: 路径折叠——write_file 落到宿主机其他位置
    失败意味着: 文件不可在 workdir 找到，隔离失效
    """
    endpoint, workdir = sandbox_server
    result = json.loads(write_execute(
        {"file_path": "output.txt", "content": "hello from write_file"},
        sandbox_endpoint=endpoint,
    ))
    assert result.get("success") is True, f"write_file 失败: {result}"
    # 宿主机对应 workdir 下应可见
    written = Path(workdir) / "output.txt"
    assert written.is_file(), f"文件未落 workdir: {workdir}"
    assert written.read_text(encoding="utf-8") == "hello from write_file", "内容不一致"


# AC-US2-001: read_file 读取沙箱内 hello.txt 返回内容
def test_read_file_returns_content(sandbox_server):
    """AC-US2-001 + TC-SBOX-061: read_file 返回内容。

    业务规则: B14 统一 MCP-like 入口，沙箱侧读文件
    错误模型: 命令泄漏——read_file 走了宿主机 open() 而非沙箱侧
    失败意味着: read_file 不走沙箱，路径双检失效
    """
    endpoint, workdir = sandbox_server
    # 先 write_file 落 hello.txt
    json.loads(write_execute(
        {"file_path": "hello.txt", "content": "hello content for read"},
        sandbox_endpoint=endpoint,
    ))
    # read_file 读回
    result = json.loads(read_execute(
        {"file_path": "hello.txt"},
        sandbox_endpoint=endpoint,
    ))
    assert "content" in result, f"read_file 未返回 content 字段: {result}"
    assert "hello content for read" in result["content"], f"内容不一致: {result}"


# AC-US2-002: read_file 传入 ../../../etc/passwd 沙箱侧双检拒绝含路径名
def test_read_file_rejects_escape(sandbox_server):
    """AC-US2-002 + TC-SBOX-023: ../etc/passwd 拒绝含路径名。

    业务规则: B4 路径双检在沙箱侧 server 做
    错误模型: 路径折叠——沙箱侧 server resolve_safe_path 漏越界
    失败意味着: 越界路径被读取，宿主机敏感文件泄漏
    """
    endpoint, workdir = sandbox_server
    result = json.loads(read_execute(
        {"file_path": "../../../etc/passwd"},
        sandbox_endpoint=endpoint,
    ))
    # 沙箱侧 server 应返回 exit_code=1 + stderr 含路径名
    assert result.get("exit_code") == 1 or "error" in result, f"越界路径未拒绝: {result}"
    # 错误信息应含路径名（M3 错误可定位）
    err_text = result.get("stderr", "") or result.get("error", "")
    assert "passwd" in err_text or "etc" in err_text, f"拒绝信息不含路径名: {result}"


# 额外：write_file 越界也拒绝
def test_write_file_rejects_escape(sandbox_server):
    """TC-SBOX-023: write_file 越界路径拒绝。"""
    endpoint, workdir = sandbox_server
    result = json.loads(write_execute(
        {"file_path": "../../../tmp/evil_escape.txt", "content": "should be rejected"},
        sandbox_endpoint=endpoint,
    ))
    assert result.get("exit_code") == 1 or "error" in result, f"越界 write 未拒绝: {result}"


# 额外：edit_file 精确替换闭环（write → edit → read）
def test_edit_file_replace_cycle(sandbox_server):
    """TC-SBOX-004: edit_file write→edit→read 闭环。"""
    endpoint, workdir = sandbox_server
    # write 原文件
    json.loads(write_execute(
        {"file_path": "edit_target.txt", "content": "old line\nnew line\n"},
        sandbox_endpoint=endpoint,
    ))
    # edit 替换 "old" → "edited"
    result_e = json.loads(edit_execute(
        {"file_path": "edit_target.txt", "old_str": "old", "new_str": "edited", "replace_all": False},
        sandbox_endpoint=endpoint,
    ))
    assert result_e.get("success") is True or result_e.get("exit_code") == 0, f"edit 失败: {result_e}"
    # read 验证替换生效
    result_r = json.loads(read_execute(
        {"file_path": "edit_target.txt"},
        sandbox_endpoint=endpoint,
    ))
    assert "edited line" in result_r.get("content", ""), f"替换未生效: {result_r}"
    assert "old line" not in result_r.get("content", ""), f"旧字符串仍存在: {result_r}"


# 额外：edit_file old_str 不存在返回明确错误
def test_edit_file_old_str_not_found(sandbox_server):
    """edit_file old_str 不存在返回明确错误含路径名。"""
    endpoint, workdir = sandbox_server
    json.loads(write_execute(
        {"file_path": "edit_no_match.txt", "content": "existing content\n"},
        sandbox_endpoint=endpoint,
    ))
    result = json.loads(edit_execute(
        {"file_path": "edit_no_match.txt", "old_str": "NONEXISTENT_STR", "new_str": "x"},
        sandbox_endpoint=endpoint,
    ))
    assert result.get("exit_code") == 1 or "error" in result, f"old_str 不存在未拒绝: {result}"
    err_text = result.get("stderr", "") or result.get("error", "")
    assert "edit_no_match.txt" in err_text or "not found" in err_text, f"错误信息不含路径名: {result}"


# 额外：edit_file old_str 不唯一且 replace_all=false 拒绝
def test_edit_file_not_unique_rejected(sandbox_server):
    """edit_file old_str 不唯一且 replace_all=false 拒绝含数量。"""
    endpoint, workdir = sandbox_server
    json.loads(write_execute(
        {"file_path": "edit_dup.txt", "content": "dup dup dup\n"},
        sandbox_endpoint=endpoint,
    ))
    result = json.loads(edit_execute(
        {"file_path": "edit_dup.txt", "old_str": "dup", "new_str": "x", "replace_all": False},
        sandbox_endpoint=endpoint,
    ))
    assert result.get("exit_code") == 1 or "error" in result, f"不唯一 old_str 未拒绝: {result}"


# 额外：edit_file replace_all=true 替换全部
def test_edit_file_replace_all(sandbox_server):
    """edit_file replace_all=true 替换全部出现。"""
    endpoint, workdir = sandbox_server
    json.loads(write_execute(
        {"file_path": "edit_all.txt", "content": "dup dup dup\n"},
        sandbox_endpoint=endpoint,
    ))
    result = json.loads(edit_execute(
        {"file_path": "edit_all.txt", "old_str": "dup", "new_str": "x", "replace_all": True},
        sandbox_endpoint=endpoint,
    ))
    assert result.get("success") is True or result.get("exit_code") == 0, f"replace_all 失败: {result}"
    result_r = json.loads(read_execute(
        {"file_path": "edit_all.txt"},
        sandbox_endpoint=endpoint,
    ))
    assert result_r.get("content", "").strip() == "x x x", f"replace_all 未全替换: {result_r}"


# 额外：向后兼容——sandbox_endpoint=None 走宿主机 open()
def test_backward_compat_none_endpoint(tmp_path):
    """sandbox_endpoint=None 时 write_file/read_file/edit_file 走宿主机路径（向后兼容）。"""
    # 切到 tmp_path 避免污染
    os.chdir(tmp_path)
    # write
    result_w = json.loads(write_execute({"file_path": "compat.txt", "content": "compat content"}, sandbox_endpoint=None))
    assert result_w.get("success") is True, f"向后兼容 write 失败: {result_w}"
    assert (tmp_path / "compat.txt").is_file(), "文件未落宿主机 cwd"
    # read
    result_r = json.loads(read_execute({"file_path": "compat.txt"}, sandbox_endpoint=None))
    assert "compat content" in result_r.get("content", ""), f"向后兼容 read 失败: {result_r}"
    # edit
    result_e = json.loads(edit_execute(
        {"file_path": "compat.txt", "old_str": "compat", "new_str": "edited", "replace_all": False},
        sandbox_endpoint=None,
    ))
    assert result_e.get("success") is True, f"向后兼容 edit 失败: {result_e}"
    assert (tmp_path / "compat.txt").read_text(encoding="utf-8").strip() == "edited content"


def test_artifact_download_is_binary_safe_and_untruncated(
    sandbox_server, tmp_path, monkeypatch: pytest.MonkeyPatch
):
    """Artifact 拉回必须保留超过 read_file 文本上限的任意原始字节。"""
    from agentflow_runtime import skill_executor as se

    endpoint, workdir = sandbox_server
    payload = b"\x89PNG\r\n\x1a\n" + bytes(range(256)) * 128
    sandbox_file = Path(workdir) / "artifact-download.png"
    sandbox_file.write_bytes(payload)

    package_dir = tmp_path / "agent"
    package_dir.mkdir()
    monkeypatch.setattr(se, "AGENT_PACKAGE_DIR", str(package_dir))
    downloaded = package_dir / sandbox_file.name
    try:
        se._download_from_sandbox(downloaded, endpoint)
        assert downloaded.is_file(), "Artifact 未从 Sandbox 拉回"
        assert downloaded.read_bytes() == payload
    finally:
        sandbox_file.unlink(missing_ok=True)


@pytest.mark.parametrize(
    ("path", "expected_status"),
    [
        ("../outside.png", 400),
        ("workspace/user_client/upload/input.png", 403),
        ("secrets.env", 415),
    ],
)
def test_artifact_download_rejects_non_artifact_paths(
    sandbox_server, path: str, expected_status: int
):
    """Artifact 端点拒绝越界路径、用户输入目录和非产物后缀。"""
    import requests

    endpoint, _ = sandbox_server
    response = requests.post(
        f"{endpoint}/artifact/download",
        json={"path": path},
        timeout=3,
    )
    assert response.status_code == expected_status



def test_local_executor_scopes_user_workspace_and_static_skills(
    sandbox_server, monkeypatch, tmp_path
):
    """Local HTTP files use one user root; skill files stay read-only."""
    from agentflow_runtime.executors.http import HTTPExecutor

    endpoint, _ = sandbox_server
    package_root = tmp_path / "outfit-agent"
    user_root = package_root / "workspace" / "user-a_conv-a"
    other_root = package_root / "workspace" / "user-b_conv-b"
    skill_file = package_root / "harness" / "skills" / "stylist" / "SKILL.md"
    user_root.mkdir(parents=True)
    other_root.mkdir(parents=True)
    skill_file.parent.mkdir(parents=True)
    (other_root / "private.md").write_text("other-user-secret", encoding="utf-8")
    skill_file.write_text("static-skill", encoding="utf-8")

    monkeypatch.setenv("AF_SANDBOX_MODE", "local")
    monkeypatch.setenv("AF_AGENT_PACKAGE_DIR", str(package_root))
    monkeypatch.setenv("AF_USER_ID", "user-a")
    monkeypatch.setenv("AF_CLIENT_ID", "conv-a")
    executor = HTTPExecutor(endpoint)

    written = executor.write_file("outputs/result.html", "<html>ok</html>")
    expected = user_root / "outputs" / "result.html"
    assert written.success is True
    assert written.path == str(expected)
    assert expected.read_text(encoding="utf-8") == "<html>ok</html>"
    assert executor.read_file(str(expected)).content == "<html>ok</html>"

    denied = executor.read_file(str(other_root / "private.md"))
    assert denied.error and "Path escape rejected" in denied.error
    assert "other-user-secret" not in denied.content

    assert executor.read_file(str(skill_file)).content == "static-skill"
    denied_write = executor.write_file(str(skill_file), "overwritten")
    assert denied_write.success is False
    assert skill_file.read_text(encoding="utf-8") == "static-skill"


def test_local_executor_list_dir_preserves_user_boundary(
    sandbox_server, monkeypatch, tmp_path
):
    """HTTP list_dir returns current-user/static entries and rejects siblings."""
    from agentflow_runtime.executors.http import HTTPExecutor

    endpoint, _ = sandbox_server
    package_root = tmp_path / "outfit-agent"
    user_root = package_root / "workspace" / "user-a_conv-a"
    other_root = package_root / "workspace" / "user-b_conv-b"
    skills_root = package_root / "harness" / "skills"
    (user_root / "history").mkdir(parents=True)
    other_root.mkdir(parents=True)
    (skills_root / "stylist").mkdir(parents=True)
    (user_root / "profile.md").write_text("profile", encoding="utf-8")

    monkeypatch.setenv("AF_SANDBOX_MODE", "local")
    monkeypatch.setenv("AF_AGENT_PACKAGE_DIR", str(package_root))
    monkeypatch.setenv("AF_USER_ID", "user-a")
    monkeypatch.setenv("AF_CLIENT_ID", "conv-a")
    executor = HTTPExecutor(endpoint)

    user_entries = executor.list_dir(str(user_root))
    assert {entry["name"] for entry in user_entries} == {"history", "profile.md"}
    skill_entries = executor.list_dir(str(skills_root))
    assert [entry["name"] for entry in skill_entries] == ["stylist"]

    denied = executor._post("list_dir", {"path": str(other_root)})
    assert denied.get("error") and "Path escape rejected" in denied["error"]
