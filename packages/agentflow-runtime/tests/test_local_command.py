"""Real child-process checks; all sensitive fixtures contain dummy values."""

import concurrent.futures
import ctypes
import json
import os
from pathlib import Path
import platform
import shlex
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest
import requests

from agentflow_runtime.security.local_command import run_local_command
from agentflow_runtime.security.secrets import _reset_registry_for_tests


@pytest.fixture(autouse=True)
def supported_platform():
    if sys.platform != "linux" or platform.machine() not in {"x86_64", "aarch64"}:
        pytest.skip("requires Linux Landlock")
    libc = ctypes.CDLL(None)
    if libc.syscall(444, 0, 0, 1) < 3:
        pytest.skip("requires Landlock ABI 3")
    try:
        ctypes.CDLL("libseccomp.so.2")
    except OSError:
        pytest.skip("requires libseccomp")
    _reset_registry_for_tests()
    yield
    _reset_registry_for_tests()


def python_command(source):
    return shlex.quote(sys.executable) + " -c " + shlex.quote(source)


@pytest.fixture
def local_server(monkeypatch, tmp_path):
    from agentflow_runtime.security.local_sandbox_auth import local_sandbox_headers

    repo = Path(__file__).resolve().parents[3]
    package = tmp_path / "agent"
    workspace = package / "workspace" / "user-a_conv-a"
    server_root = tmp_path / "server"
    server_root.mkdir()
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    endpoint = f"http://127.0.0.1:{port}"
    monkeypatch.setenv("AF_SANDBOX_MODE", "local")
    monkeypatch.setenv("AF_SANDBOX_ENDPOINT", endpoint)
    monkeypatch.setenv("AF_SANDBOX_WORKSPACE", str(package))
    monkeypatch.setenv("AF_AGENT_PACKAGE_DIR", str(package))
    monkeypatch.setenv("AF_USER_ID", "user-a")
    monkeypatch.setenv("AF_CLIENT_ID", "conv-a")
    monkeypatch.setenv("UNUSUAL_VENDOR_API_KEY", "dummy-http-test-secret-12345678")
    monkeypatch.delenv("AF_ALLOW_UNSAFE_LOCAL_SECRET_TOOLS", raising=False)
    env = dict(os.environ, PYTHONPATH=str(repo / "packages/agentflow-runtime/src"))
    with (tmp_path / "server.log").open("w+") as log:
        child = subprocess.Popen(
            [sys.executable, str(repo / "docker/sandbox/http_server/sandbox_http_server.py"),
             "--host", "127.0.0.1", "--port", str(port), "--security-profile", "restricted-local"],
            cwd=server_root, env=env, stdin=subprocess.DEVNULL,
            stdout=log, stderr=log, start_new_session=True,
        )
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                assert child.poll() is None, "local server exited during startup"
                try:
                    local_sandbox_headers(endpoint)
                    response = requests.get(endpoint, proxies={"http": "", "https": ""}, timeout=1)
                    if response.status_code == 404:
                        break
                except (OSError, ValueError, requests.RequestException):
                    pass
                time.sleep(0.02)
            else:
                pytest.fail("local server did not become ready")
            yield endpoint, package, workspace, server_root
        finally:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
    from agentflow_runtime.security.local_sandbox_auth import _directory
    assert not (_directory() / str(port)).exists()


def test_real_registry_local_bash_with_credentials(local_server):
    from agentflow_runtime.executors.http import HTTPExecutor
    from agentflow_runtime.harness.tools import bash
    from agentflow_runtime.tool_registry import _LocalToolRegistry

    endpoint, package, workspace, _ = local_server
    registry = _LocalToolRegistry.__new__(_LocalToolRegistry)
    registry._tools, registry._modules, registry._mcp_bridges = [], {"bash": bash}, {}
    result = json.loads(registry.execute("bash", {"command": python_command(
        "import json,os; from pathlib import Path; "
        "assert 'UNUSUAL_VENDOR_API_KEY' not in os.environ; "
        "assert 'AF_SANDBOX_ENDPOINT' not in os.environ; "
        "p=Path('result.json'); p.write_text(json.dumps({'sum':19})); "
        "print(json.loads(p.read_text())['sum'])"
    )}))
    assert result["exit_code"] == 0 and result["stdout"] == "19\n", result
    assert workspace.is_dir()
    assert json.loads((workspace / "result.json").read_text()) == {"sum": 19}
    read = HTTPExecutor(endpoint).read_file("result.json")
    assert read.error is None and json.loads(read.content) == {"sum": 19}
    assert not (package / "result.json").exists()
    blocked = json.loads(registry.execute("bash", {"command": "echo dummy-http-test-secret-12345678"}))
    assert blocked["status"] == "security_policy_blocked"


def test_real_local_server_requires_auth_for_every_post(local_server):
    from agentflow_runtime.security.local_sandbox_auth import HEADER

    endpoint, _, _, _ = local_server
    for path in ("/exec", "/upload", "/artifact/upload", "/artifact/download", "/pty/open"):
        for headers in ({}, {HEADER: "wrong"}):
            response = requests.post(endpoint + path, json={}, headers=headers,
                                     proxies={"http": "", "https": ""}, timeout=2)
            assert response.status_code == 401
            assert response.json()["error"]["code"] == "LOCAL_SANDBOX_AUTH_REQUIRED"


def test_real_local_bash_cannot_bypass_boundary_via_http(local_server, tmp_path):
    from agentflow_runtime.executors.http import HTTPExecutor
    from agentflow_runtime.security.local_sandbox_auth import _directory

    endpoint, _, _, _ = local_server
    auth_path = _directory() / endpoint.rsplit(":", 1)[1]
    result = HTTPExecutor(endpoint).exec_command(python_command(
        "import urllib.request,urllib.error; from pathlib import Path\n"
        f"try: Path({str(auth_path)!r}).read_text()\n"
        "except PermissionError: pass\n"
        "else: raise AssertionError('private header readable')\n"
        f"request=urllib.request.Request({endpoint + '/exec'!r},data=b'{{}}')\n"
        "try: urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request,timeout=2)\n"
        "except urllib.error.HTTPError as exc: assert exc.code==401\n"
        "else: raise AssertionError('unauthenticated execution')\n"
        "print('boundary intact')"
    ))
    assert result.exit_code == 0 and result.stdout == "boundary intact\n", result
    failed = HTTPExecutor(endpoint).exec_command("printf forbidden", workdir=str(tmp_path))
    assert failed.error and not failed.stdout


def test_real_local_bash_preserves_failure_and_timeout(local_server):
    from agentflow_runtime.executors.http import HTTPExecutor

    executor = HTTPExecutor(local_server[0])
    failed = executor.exec_command("printf expected-error >&2; exit 7")
    assert failed.exit_code == 7 and failed.stderr == "expected-error" and failed.error
    timed_out = executor.exec_command("sleep 10", timeout=0.2)
    assert timed_out.exit_code == -1 and "timed out" in timed_out.stderr and timed_out.error


def test_real_local_media_auth_and_cpp_curl_header_protocol(local_server):
    import base64
    from agentflow_runtime.security.local_sandbox_auth import _directory
    from agentflow_runtime.security.trusted_media import _post_json

    endpoint, _, workspace, server_root = local_server
    content = b"dummy media bytes"
    uploaded = _post_json(endpoint, "/artifact/upload", {
        "path": "outputs/example.png", "content_b64": base64.b64encode(content).decode(),
    })
    assert uploaded["written"]
    downloaded = _post_json(endpoint, "/artifact/download", {"path": "outputs/example.png"})
    assert base64.b64decode(downloaded["content_b64"]) == content
    assert (server_root / "outputs/example.png").read_bytes() == content
    workspace.mkdir(parents=True, exist_ok=True)
    # Same argv/header-file protocol as C++ LocalSandbox; token is not in argv.
    result = subprocess.run([
        "curl", "-s", "--noproxy", "*", "-m", "5", "-X", "POST",
        "-H", "Content-Type: application/json",
        "-H", "@" + str(_directory() / endpoint.rsplit(":", 1)[1]),
        "--data-binary", "@-", endpoint + "/exec",
    ], input=json.dumps({"tool": "bash", "tool_params": {"command": "printf cpp-auth-ok"},
                         "opts": {"working_dir": str(workspace)}}),
       capture_output=True, text=True, timeout=10)
    assert result.returncode == 0
    parsed = json.loads(result.stdout)
    assert parsed["exit_code"] == 0 and parsed["stdout"] == "cpp-auth-ok"


def test_real_local_artifact_download_uses_auth(local_server, monkeypatch, tmp_path):
    from agentflow_runtime import artifact_pipeline

    endpoint, _, _, server_root = local_server
    (server_root / "outputs").mkdir()
    (server_root / "outputs/image.png").write_bytes(b"dummy image")
    destination_root = tmp_path / "download"
    monkeypatch.setattr(artifact_pipeline, "_pkg_dir", lambda: str(destination_root))
    destination = destination_root / "outputs/image.png"
    artifact_pipeline._download_from_sandbox(destination, endpoint)
    assert destination.read_bytes() == b"dummy image"


def test_local_auth_is_not_forwarded_on_redirect(monkeypatch, tmp_path):
    from agentflow_runtime.executors.http import HTTPExecutor
    from agentflow_runtime.security.local_sandbox_auth import LocalSandboxAuth
    from agentflow_runtime.security.trusted_media import _post_json, TrustedMediaError

    seen = []

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            seen.append(self.path)
            self.send_response(302)
            self.send_header("Location", "/redirect-target")
            self.end_headers()

        def do_GET(self):
            seen.append(self.path)
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b'{}')

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    auth = LocalSandboxAuth(server.server_port)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    monkeypatch.setenv("AF_SANDBOX_MODE", "local")
    monkeypatch.setenv("AF_AGENT_PACKAGE_DIR", str(tmp_path))
    monkeypatch.setenv("AF_USER_ID", "a")
    monkeypatch.setenv("AF_CLIENT_ID", "b")
    monkeypatch.setenv("HTTP_PROXY", "http://127.0.0.1:1")
    monkeypatch.setenv("http_proxy", "http://127.0.0.1:1")
    endpoint = f"http://127.0.0.1:{server.server_port}"
    try:
        with pytest.raises(TrustedMediaError, match="HTTP 302"):
            _post_json(endpoint, "/artifact/download", {})
        assert HTTPExecutor(endpoint).exec_command("printf forbidden").error
        assert seen == ["/artifact/download", "/exec"]
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
        auth.close()


def test_shell_and_json_with_credentials(monkeypatch, tmp_path):
    monkeypatch.setenv("UNUSUAL_VENDOR_API_KEY", "dummy-local-test-secret-12345678")
    monkeypatch.delenv("AF_ALLOW_UNSAFE_LOCAL_SECRET_TOOLS", raising=False)
    result = run_local_command(python_command(
        "import json,os; from pathlib import Path; "
        "assert 'UNUSUAL_VENDOR_API_KEY' not in os.environ; "
        "p=Path('result.json'); p.write_text(json.dumps({'sum':12+7})); "
        "print(json.loads(p.read_text())['sum'])"
    ), str(tmp_path))
    assert result["exit_code"] == 0, result
    assert result["stdout"] == "19\n"
    assert json.loads((tmp_path / "result.json").read_text()) == {"sum": 19}
    assert "dummy-local-test-secret" not in json.dumps(result)


@pytest.mark.parametrize("via_link", [False, True])
def test_outside_credential_files_are_unreadable(tmp_path, via_link):
    root = tmp_path / "workspace"
    root.mkdir()
    credential = tmp_path / ".env"
    credential.write_text("dummy-local-test-secret-87654321")
    target = credential
    if via_link:
        target = root / "harmless.txt"
        target.symlink_to(credential)
    result = run_local_command("cat " + shlex.quote(str(target)), str(root))
    assert result["exit_code"] != 0, result
    assert "Permission denied" in result["stderr"]
    assert "dummy-local-test-secret" not in json.dumps(result)


def test_proc_environment_and_memory_cannot_be_opened(tmp_path):
    result = run_local_command(python_command(
        "from pathlib import Path\n"
        "for name in ['/proc/self/environ','/proc/self/mem']:\n"
        " try: Path(name).open('rb')\n"
        " except PermissionError: continue\n"
        " raise AssertionError('process file readable')\n"
        "print('denied')"
    ), str(tmp_path))
    assert result["exit_code"] == 0, result
    assert result["stdout"] == "denied\n"


def test_outside_write_and_metadata_are_denied(tmp_path):
    root = tmp_path / "workspace"
    root.mkdir()
    outside = tmp_path / "outside.txt"
    outside.write_text("unchanged")
    mode = outside.stat().st_mode
    result = run_local_command(python_command(
        f"import os; from pathlib import Path; p=Path({str(outside)!r})\n"
        "for op in [lambda:p.write_text('changed'),lambda:os.chmod(p,0o600)]:\n"
        " try: op()\n"
        " except PermissionError: continue\n"
        " raise AssertionError('outside mutation succeeded')\n"
        "print('denied')"
    ), str(root))
    assert result["exit_code"] == 0, result
    assert outside.read_text() == "unchanged"
    assert outside.stat().st_mode == mode


def test_process_control_and_unix_socket_are_denied(tmp_path):
    result = run_local_command(python_command(
        "import os,socket\n"
        "for op in [lambda:os.kill(os.getppid(),0),os.setsid,lambda:socket.socket(socket.AF_UNIX)]:\n"
        " try: op()\n"
        " except PermissionError: continue\n"
        " raise AssertionError('process restriction missing')\n"
        "print('denied')"
    ), str(tmp_path))
    assert result["exit_code"] == 0, result


def test_network_is_not_disabled(tmp_path):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b'{"network":"ok"}')

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        url = f"http://127.0.0.1:{server.server_port}/fixture"
        result = run_local_command(python_command(
            f"import urllib.request; print(urllib.request.urlopen({url!r},timeout=3).read().decode())"
        ), str(tmp_path))
        assert result["exit_code"] == 0, result
        assert json.loads(result["stdout"]) == {"network": "ok"}
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def test_parallel_workspaces_do_not_cross(tmp_path):
    left, right = tmp_path / "left", tmp_path / "right"
    left.mkdir(); right.mkdir()
    (left / "identity").write_text("left")
    (right / "identity").write_text("right")
    def run(root, other):
        return run_local_command(python_command(
            f"from pathlib import Path; print(Path('identity').read_text()); "
            f"Path({str(other / 'identity')!r}).read_text()"
        ), str(root))
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        futures = [pool.submit(run, left, right), pool.submit(run, right, left)]
        for identity, future in zip(("left", "right"), futures):
            result = future.result()
            assert result["stdout"] == identity + "\n"
            assert result["exit_code"] != 0
            assert "PermissionError" in result["stderr"]


@pytest.mark.parametrize("timeout,command", [(0.25, "(sleep 1; touch late) & sleep 5"),
                                             (5, "(sleep 1; touch late) & true")])
def test_background_children_are_cleaned(tmp_path, timeout, command):
    started = time.monotonic()
    result = run_local_command(command, str(tmp_path), timeout)
    assert time.monotonic() - started < 3, result
    assert result["exit_code"] == (-1 if timeout < 1 else 0), result
    time.sleep(1.1)
    assert not (tmp_path / "late").exists()


def test_invalid_workspace_and_timeout_do_not_execute(tmp_path):
    for root, timeout in [("/", 30), (str(tmp_path), 0), (str(tmp_path), float('nan'))]:
        result = run_local_command("printf forbidden", root, timeout)
        assert result["exit_code"] != 0
        assert not result["stdout"]


def test_isolation_failure_never_executes_shell(monkeypatch, capsys, tmp_path):
    import io
    from agentflow_runtime.security import local_command
    monkeypatch.setattr(local_command.platform, "machine", lambda: "unsupported")
    monkeypatch.setattr(sys, "stdin", io.StringIO(json.dumps({"root": str(tmp_path), "command": "echo unsafe"})))
    monkeypatch.setattr(os, "execve", lambda *_args: pytest.fail("shell must not execute"))
    with pytest.raises(SystemExit) as exited:
        local_command._main()
    assert exited.value.code == 126
    assert "security_policy_blocked" in capsys.readouterr().err


def test_real_image_dimensions(tmp_path):
    pytest.importorskip("PIL")
    result = run_local_command(python_command(
        "from PIL import Image; "
        "Image.new('RGB',(41,29)).save('中文图片.png'); "
        "print(Image.open('中文图片.png').size)"
    ), str(tmp_path))
    assert result["exit_code"] == 0, result
    assert result["stdout"] == "(41, 29)\n"


def test_local_auth_is_private_and_not_visible_to_commands(monkeypatch, tmp_path):
    from agentflow_runtime.security import local_sandbox_auth as auth
    private = tmp_path / "private"
    private.mkdir(mode=0o700)
    monkeypatch.setattr(auth, "_directory", lambda: private)
    monkeypatch.setenv("AF_SANDBOX_MODE", "local")
    credential = auth.LocalSandboxAuth(54321)
    root = tmp_path / "workspace"
    root.mkdir()
    try:
        headers = auth.local_sandbox_headers("http://127.0.0.1:54321")
        assert credential.accepts(headers[auth.HEADER])
        assert not credential.accepts("wrong-token")
        assert credential.path.stat().st_mode & 0o777 == 0o600
        for endpoint in ("http://remote.invalid:54321", "https://127.0.0.1:54321", "http://127.0.0.1:54321/extra"):
            with pytest.raises(ValueError):
                auth.local_sandbox_headers(endpoint)
        result = run_local_command("cat " + shlex.quote(str(credential.path)), str(root))
        assert result["exit_code"] != 0
        assert credential.token not in json.dumps(result)
        # A command cannot enter the auth directory indirectly through a link.
        (root / "link").symlink_to(private, target_is_directory=True)
        result = run_local_command("cat link/54321", str(root))
        assert result["exit_code"] != 0
        assert credential.token not in json.dumps(result)
        monkeypatch.setenv("AF_SANDBOX_MODE", "docker")
        assert auth.local_sandbox_headers("https://other.invalid") == {}
    finally:
        credential.close()
    assert not credential.path.exists()


def test_local_auth_rotation_does_not_delete_new_listener_header(monkeypatch, tmp_path):
    from agentflow_runtime.security import local_sandbox_auth as auth
    monkeypatch.setattr(auth, "_directory", lambda: tmp_path)
    old = auth.LocalSandboxAuth(54321)
    new = auth.LocalSandboxAuth(54321)
    old.close()
    assert new.path.exists()
    new.close()
    assert not new.path.exists()


def test_local_auth_rejects_invalid_headers_without_raising():
    from agentflow_runtime.security.local_sandbox_auth import LocalSandboxAuth
    auth = LocalSandboxAuth.__new__(LocalSandboxAuth)
    auth.token = "0" * 64
    assert auth.accepts(auth.token)
    for value in ("测试", "é" * 64, "wrong-token", "0" * 65, None, b"0" * 64):
        assert not auth.accepts(value)


def test_local_auth_rejects_invalid_ports_before_filesystem_access(monkeypatch):
    from agentflow_runtime.security import local_sandbox_auth as auth
    monkeypatch.setattr(auth, "_directory", lambda: pytest.fail("invalid port must not access files"))
    for port in (0, -1, 65536, True, "invalid", "../escape", 1.5, None):
        with pytest.raises(ValueError, match="listener port"):
            auth.LocalSandboxAuth(port)
