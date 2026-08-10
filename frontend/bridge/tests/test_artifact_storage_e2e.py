"""E2E 测试：Bridge 作为产物存储中心（Phase 2）。

Bridge 自身实现 upload/download 端点，不再转发到沙箱 HTTP server。
见 docs/design/features/sandbox/artifact-storage.md §3.3。
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from routes import artifacts


# ═══════════════════════════════════════════════════════════════════
#  Fixtures
# ═══════════════════════════════════════════════════════════════════


@pytest.fixture
def bridge_app(tmp_path: Path) -> FastAPI:
    """创建一个 bridge 的 FastAPI 应用，存储目录指向临时目录。"""
    app = FastAPI()
    app.include_router(artifacts.router)
    # 注入存储目录
    artifacts._ARTIFACTS_STORAGE_DIR = tmp_path / "artifacts_storage"
    return app


@pytest.fixture
def client(bridge_app: FastAPI) -> TestClient:
    return TestClient(bridge_app)


# ═══════════════════════════════════════════════════════════════════
#  Bridge 存储端点测试
# ═══════════════════════════════════════════════════════════════════


class TestBridgeStorageEndpoints:
    """验证 bridge 的 upload/download 端点。"""

    def test_upload_and_download_roundtrip(self, client: TestClient, tmp_path: Path):
        """上传文件 → 下载文件 → 验证内容一致。"""
        upload_url = "/api/artifacts/upload"
        body = {
            "agent_name": "test-agent",
            "session_id": "user_test_client_test",
            "files": [
                {"path": "outputs/look.png", "data_b64": "ZmFrZS1wbmctY29udGVudA=="},
            ],
        }
        resp = client.post(upload_url, json=body)
        assert resp.status_code == 200, f"Upload failed: {resp.status_code} {resp.text}"
        data = resp.json()
        assert "artifact_ids" in data
        assert len(data["artifact_ids"]) == 1
        artifact_id = data["artifact_ids"][0]
        assert artifact_id == "test-agent/user_test_client_test/outputs/look.png"

        # 验证文件已存到磁盘
        stored = tmp_path / "artifacts_storage" / artifact_id
        assert stored.is_file()
        assert stored.read_bytes() == b"fake-png-content"

        # 下载
        resp = client.get(f"/api/artifacts/{artifact_id}")
        assert resp.status_code == 200
        assert resp.content == b"fake-png-content"

    def test_upload_multiple_files(self, client: TestClient, tmp_path: Path):
        """一次上传多个文件。"""
        import base64
        pdf_b64 = base64.b64encode(b"pdf-content").decode("ascii")
        png_b64 = base64.b64encode(b"png-content").decode("ascii")
        body = {
            "agent_name": "test-agent",
            "session_id": "user_test_client_test",
            "files": [
                {"path": "outputs/report.pdf", "data_b64": pdf_b64},
                {"path": "outputs/chart.png", "data_b64": png_b64},
            ],
        }
        resp = client.post("/api/artifacts/upload", json=body)
        assert resp.status_code == 200
        data = resp.json()
        assert len(data["artifact_ids"]) == 2
        assert (tmp_path / "artifacts_storage" / "test-agent/user_test_client_test/outputs/report.pdf").is_file()
        assert (tmp_path / "artifacts_storage" / "test-agent/user_test_client_test/outputs/chart.png").is_file()

    def test_download_not_found(self, client: TestClient):
        """下载不存在的产物返回 404。"""
        resp = client.get("/api/artifacts/nonexistent/file.png")
        assert resp.status_code == 404

    def test_upload_binary_roundtrip(self, client: TestClient, tmp_path: Path):
        """P0-1: 二进制产物(PNG)通过 base64 上传下载，字节必须完全一致。

        验证 I-binary-safe-transfer 不变量。
        """
        import base64
        binary_payload = b"\x89PNG\r\n\x1a\n" + b"\x00\x01\x02\xff\xfe\x00\x80"
        body = {
            "agent_name": "test-agent",
            "session_id": "user_bin_client_bin",
            "files": [
                {
                    "path": "outputs/look.png",
                    "data_b64": base64.b64encode(binary_payload).decode("ascii"),
                },
            ],
        }
        resp = client.post("/api/artifacts/upload", json=body)
        assert resp.status_code == 200, f"Upload failed: {resp.status_code} {resp.text}"
        artifact_id = resp.json()["artifact_ids"][0]
        stored = tmp_path / "artifacts_storage" / artifact_id
        assert stored.read_bytes() == binary_payload, "Stored bytes mismatch"
        # 下载也必须字节一致
        resp = client.get(f"/api/artifacts/{artifact_id}")
        assert resp.status_code == 200
        assert resp.content == binary_payload, "Downloaded bytes mismatch"

    def test_upload_path_traversal_rejected(self, client: TestClient):
        """P0-2: POST upload 的 file_path 字段含 `..` 必须被拒。"""
        body = {
            "agent_name": "test-agent",
            "session_id": "user_test_client_test",
            "files": [{"path": "../../etc/passwd", "data_b64": "AAA="}],
        }
        resp = client.post("/api/artifacts/upload", json=body)
        assert resp.status_code == 400, f"Expected 400, got {resp.status_code} {resp.text}"

    def test_upload_absolute_path_rejected(self, client: TestClient):
        """P0-2: POST upload 的 file_path 字段为绝对路径必须被拒。"""
        body = {
            "agent_name": "test-agent",
            "session_id": "user_test_client_test",
            "files": [{"path": "/etc/passwd", "data_b64": "AAA="}],
        }
        resp = client.post("/api/artifacts/upload", json=body)
        assert resp.status_code == 400, f"Expected 400, got {resp.status_code} {resp.text}"

    def test_upload_bad_agent_name_rejected(self, client: TestClient):
        """P0-2: agent_name 含 `/` 或 `..` 必须被拒。"""
        body = {
            "agent_name": "bad/../agent",
            "session_id": "user_test_client_test",
            "files": [{"path": "outputs/x.png", "data_b64": "AAA="}],
        }
        resp = client.post("/api/artifacts/upload", json=body)
        assert resp.status_code == 400, f"Expected 400, got {resp.status_code} {resp.text}"

    def test_upload_bad_session_id_rejected(self, client: TestClient):
        """P0-2: session_id 含 `..` 必须被拒。"""
        body = {
            "agent_name": "test-agent",
            "session_id": "..",
            "files": [{"path": "outputs/x.png", "data_b64": "AAA="}],
        }
        resp = client.post("/api/artifacts/upload", json=body)
        assert resp.status_code == 400, f"Expected 400, got {resp.status_code} {resp.text}"


# ═══════════════════════════════════════════════════════════════════
#  Bridge 本地文件系统读取测试
# ═══════════════════════════════════════════════════════════════════


class TestBridgeLocalFS:
    """验证 bridge 从本地文件系统读取产物（legacy fallback 路径）。"""

    @pytest.fixture
    def app_with_local_fs(self, tmp_path: Path) -> FastAPI:
        app = FastAPI()
        app.include_router(artifacts.router)
        # 存储目录为空，模拟无存储服务
        artifacts._ARTIFACTS_STORAGE_DIR = tmp_path / "empty_storage"
        # 三态模型：产物属运行态数据，落点在 ③ workspace 实例目录
        # _resolve_artifact 主路径从 agent_workspace_path 解析，故 fixture 设置 ③
        workspace_root = tmp_path / "workspace"
        workspace_root.mkdir()
        artifacts.settings.agent_workspace_dir = str(workspace_root)
        local_file = workspace_root / "test-pkg/outputs/look.png"
        local_file.parent.mkdir(parents=True, exist_ok=True)
        local_file.write_bytes(b"local-fallback-content")
        return app

    def test_artifact_fallback_to_local_fs(self, app_with_local_fs: FastAPI):
        """存储服务不可用时降级到本地文件系统。"""
        client = TestClient(app_with_local_fs)
        resp = client.get("/api/artifacts/test-pkg/outputs/look.png")
        assert resp.status_code == 200, f"Expected 200, got {resp.status_code}"
        assert resp.content == b"local-fallback-content"

    def test_artifact_not_found(self, client: TestClient):
        """产物不存在时返回 404。"""
        # 使用默认 client（无本地文件）
        resp = client.get("/api/artifacts/nonexistent/outputs/missing.png")
        assert resp.status_code == 404


# ═══════════════════════════════════════════════════════════════════
#  skill_executor 上传测试
# ═══════════════════════════════════════════════════════════════════


class TestUploadToStorage:
    """验证 _upload_artifacts_to_storage 函数上传到 bridge。"""

    def test_upload_artifacts_to_storage(self, client: TestClient, tmp_path: Path,
                                          monkeypatch: pytest.MonkeyPatch):
        """验证上传函数正确地将产物上传到 bridge 的 upload 端点。"""
        monkeypatch.setenv("AF_ARTIFACT_STORAGE_URL", "http://test-bridge")
        monkeypatch.setenv("AF_SESSION_ID", "user_test_client_test")
        # 模拟 AGENT_PACKAGE_DIR
        agent_dir = tmp_path / "test-agent"
        agent_dir.mkdir()
        import agentflow_runtime.skill_executor as se
        monkeypatch.setattr(se, "AGENT_PACKAGE_DIR", str(agent_dir))

        # 创建测试产物文件
        outputs_dir = agent_dir / "outputs"
        outputs_dir.mkdir()
        test_file = outputs_dir / "test-look.png"
        test_file.write_bytes(b"test-artifact-content")

        # 调用上传函数（默认 URL 指向 bridge）
        se._upload_artifacts_to_storage([str(test_file)])

    def test_upload_to_real_bridge_roundtrip(self, bridge_app: FastAPI, tmp_path: Path,
                                              monkeypatch: pytest.MonkeyPatch):
        """P0: 设置 AF_ARTIFACT_STORAGE_URL 指向 bridge，验证上传+下载全链路。

        验证设计文档 §8.3 中 AF_ARTIFACT_STORAGE_URL 已设置时的完整链路：
        skill_executor 上传 → bridge 存储 → artifact_id 可下载。
        """
        import struct, zlib, json
        from unittest.mock import patch

        # 使用 FastAPI TestClient 作为后端，将 urllib.request 请求转发到 bridge_app
        bridge_client = TestClient(bridge_app)

        def _patched_urlopen(req, timeout=None):
            """将 urllib.request.Request 通过 TestClient 转发到 bridge_app。"""
            method = req.method
            url = str(req.full_url)
            # 从完整 URL 中提取路径部分
            from urllib.parse import urlparse
            path = urlparse(url).path
            headers = dict(req.headers)
            body = req.data
            # 通过 TestClient 发送请求
            resp = bridge_client.request(method, path, content=body, headers=headers)
            from io import BytesIO
            class MockResp:
                def __init__(self, status, data_bytes):
                    self.status = status
                    self.data = BytesIO(data_bytes)
                def read(self):
                    return self.data.read()
                def __enter__(self):
                    return self
                def __exit__(self, *args):
                    pass
            return MockResp(resp.status_code, resp.content)

        # 设置 AF_ARTIFACT_STORAGE_URL 指向 bridge
        monkeypatch.setenv("AF_ARTIFACT_STORAGE_URL", "http://bridge-test")
        monkeypatch.setenv("AF_SESSION_ID", "user_test_client_test")

        # 模拟 AGENT_PACKAGE_DIR
        agent_dir = tmp_path / "test-agent"
        agent_dir.mkdir()
        import agentflow_runtime.skill_executor as se
        monkeypatch.setattr(se, "AGENT_PACKAGE_DIR", str(agent_dir))

        # 创建测试产物文件（合法 PNG 数据，> 100 bytes）
        outputs_dir = agent_dir / "outputs"
        outputs_dir.mkdir()
        test_file = outputs_dir / "test-look.png"
        W, H = 50, 50
        raw = b'\x00' + b'\xff\x00\x00' * W
        img_data = raw * H
        png_bytes = (
            b'\x89PNG\r\n\x1a\n'
            + struct.pack('>I', 13) + b'IHDR' + struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)
            + struct.pack('>I', zlib.crc32(b'IHDR' + struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) & 0xffffffff)
            + struct.pack('>I', len(img_data)) + b'IDAT' + zlib.compress(img_data)
            + struct.pack('>I', zlib.crc32(b'IDAT' + zlib.compress(img_data)) & 0xffffffff)
            + struct.pack('>I', 0) + b'IEND'
            + struct.pack('>I', zlib.crc32(b'IEND') & 0xffffffff)
        )
        test_file.write_bytes(png_bytes)
        assert len(png_bytes) > 100, f"PNG too small: {len(png_bytes)} bytes"

        # 用 patch 替换 urllib.request.urlopen 为 TestClient 代理
        import urllib.request
        with patch.object(urllib.request, "urlopen", _patched_urlopen):
            se._upload_artifacts_to_storage([str(test_file)])

        # 验证：可通过 bridge 的 GET 端点下载
        artifact_id = f"test-agent/user_test_client_test/outputs/test-look.png"
        resp = bridge_client.get(f"/api/artifacts/{artifact_id}")
        assert resp.status_code == 200, f"Download failed: {resp.status_code} {resp.text}"
        assert resp.content == png_bytes, "Downloaded PNG bytes mismatch"

    def test_upload_skipped_when_no_session(self, monkeypatch: pytest.MonkeyPatch):
        """未设置 AF_SESSION_ID 时静默跳过。"""
        monkeypatch.delenv("AF_SESSION_ID", raising=False)
        import agentflow_runtime.skill_executor as se
        se._upload_artifacts_to_storage([])


# ═══════════════════════════════════════════════════════════════════
#  沙箱产物下载测试（TDD Red）
# ═══════════════════════════════════════════════════════════════════


class TestSandboxArtifactDownload:
    """P1: 沙箱内生成的产物文件应能下载到宿主机。

    当前 bug：SandboxHelpers::bash 执行后，产物文件留在沙箱内，
    _ensure_artifacts_in_package 在宿主机上找不到文件而跳过。
    修复后，_ensure_artifacts_in_package 应尝试从沙箱 HTTP server
    下载缺失的文件。
    """

    def test_download_missing_file_from_sandbox(self, tmp_path: Path):
        """Red: 调用 _ensure_artifacts_in_package 时，宿主机上不存在的文件
        应从沙箱 HTTP server 下载。当前无此逻辑，测试应失败。"""
        import threading, http.server, json, urllib.request
        from agentflow_runtime import skill_executor as se

        # 在沙箱内创建的文件路径（宿主机上不存在）
        sandbox_file_rel = "outputs/test-artifact.png"
        sandbox_content = b"fake-sandbox-file-content"

        # 启动 mock 沙箱 HTTP server
        class MockSandboxHandler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                if self.path == "/exec":
                    length = int(self.headers.get("Content-Length", 0))
                    body = json.loads(self.rfile.read(length).decode() if length > 0 else "{}")
                    tool = body.get("tool", "")
                    params = body.get("params", {})
                    if tool == "read_file":
                        path = params.get("path", "")
                        if path == sandbox_file_rel:
                            resp = json.dumps({
                                "stdout": sandbox_content.decode("latin-1"),
                                "stderr": "", "exit_code": 0, "tool": "read_file"
                            }).encode()
                            self.send_response(200)
                            self.send_header("Content-Type", "application/json")
                            self.end_headers()
                            self.wfile.write(resp)
                            return
                self.send_response(404)
                self.end_headers()

            def log_message(self, *args):
                pass

        server = http.server.HTTPServer(("127.0.0.1", 0), MockSandboxHandler)
        port = server.server_port
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        sandbox_url = f"http://127.0.0.1:{port}"

        # 模拟 AGENT_PACKAGE_DIR
        agent_dir = tmp_path / "test-agent"
        agent_dir.mkdir()
        old_pkg_dir = se.AGENT_PACKAGE_DIR
        se.AGENT_PACKAGE_DIR = str(agent_dir)
        old_sandbox_url = os.environ.get("AF_ARTIFACT_STORAGE_URL")
        os.environ["AF_ARTIFACT_STORAGE_URL"] = sandbox_url

        try:
            # 调用 _ensure_artifacts_in_package，文件不在宿主机上
            abs_path = str(agent_dir / sandbox_file_rel)
            result = se._ensure_artifacts_in_package([abs_path])

            # 期望：文件被从沙箱下载到宿主机
            downloaded = agent_dir / sandbox_file_rel
            assert downloaded.is_file(), (
                f"RED: 文件未从沙箱下载到宿主机.\n"
                f"  sandbox_url={sandbox_url}\n"
                f"  expected={downloaded}\n"
                f"  result={result}\n"
                f"  修复 _ensure_artifacts_in_package 后此测试应通过"
            )
            assert downloaded.read_bytes() == sandbox_content, (
                f"下载的文件内容不匹配"
            )
        finally:
            se.AGENT_PACKAGE_DIR = old_pkg_dir
            server.shutdown()