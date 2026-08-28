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
        # E2E-3 契约对齐(2026-08-12):上传侧 _upload_artifacts_to_storage 走
        # _artifact_session_id() 拼 {user_id}_{client_id},不走 AF_SESSION_ID(执行槽位).
        # 测试 env var 必须对齐新契约,否则 URL 段与落盘段错位 → 404.
        monkeypatch.setenv("AF_USER_ID", "user_test")
        monkeypatch.setenv("AF_CLIENT_ID", "client_test")

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
#  E2E-4 不变量测试:转存失败可观测性
# ═══════════════════════════════════════════════════════════════════


class TestUploadArtifactFailureObservability:
    """E2E-4 锁死: 转存失败必须 logger.error 带可定位信息,不静默吞.

    设计文档输出侧方法论 §11.4 不变量 E2E-4:
    - 每个环节失败(沙箱拉回失败/转存失败/URL 化失败)必须打日志带可定位信息
      (artifact_id、源路径、错误原因),不静默吞
    - 当前转存失败已静默降级(logger.error 不抛),需评估是否在终态改为抛异常

    本测试类覆盖三种失败分支,各自断言 logger.error 被调用且带可定位信息:
    - 读取失败(read_failed):文件存在但 read_bytes 抛 OSError(权限/IO 错误)
    - 上传失败(upload.failed):bridge 端返回非 200 或网络不可达
    - 缺失文件(skip_missing):文件不存在,走 warning 而非 error
    """

    def test_upload_read_failure_is_observable(self, tmp_path: Path,
                                                monkeypatch: pytest.MonkeyPatch,
                                                caplog):
        """E2E-4 ①: 文件存在但 read_bytes 抛 OSError 时,
        必须 logging.error 带 path + err,不静默吞.
        """
        import logging as _logging
        from agentflow_runtime import skill_executor as se

        agent_dir = tmp_path / "test-agent"
        outputs_dir = agent_dir / "outputs"
        outputs_dir.mkdir(parents=True)
        # 创建一个存在但会被 mock 抛 OSError 的文件
        broken_file = outputs_dir / "broken.png"
        broken_file.write_bytes(b"fake")

        monkeypatch.setattr(se, "AGENT_PACKAGE_DIR", str(agent_dir))
        monkeypatch.setenv("AF_USER_ID", "user_abc")
        monkeypatch.setenv("AF_CLIENT_ID", "client_xyz")
        monkeypatch.setenv("AF_ARTIFACT_STORAGE_URL", "http://localhost:8199")

        # mock Path.read_bytes 抛 OSError(权限错误场景)
        def _raise_oserror(self):
            raise OSError("Permission denied (mock)")
        monkeypatch.setattr("pathlib.Path.read_bytes", _raise_oserror)

        with caplog.at_level(_logging.ERROR, logger="root"):
            se._upload_artifacts_to_storage([str(broken_file)])

        # E2E-4 断言:必须有 error 级日志,且含 path + err 可定位信息
        error_records = [r for r in caplog.records if r.levelno == _logging.ERROR]
        assert error_records, (
            "E2E-4 违反: read_failed 无 error 级日志,失败被静默吞\n"
            "  必须有 logging.error 带 path + err"
        )
        log_text = " ".join(r.getMessage() for r in error_records)
        assert "artifact.upload.read_failed" in log_text or "read_failed" in log_text, (
            f"E2E-4 违反: error 日志缺 read_failed 标记\n  log={log_text!r}"
        )
        assert str(broken_file) in log_text or "broken.png" in log_text, (
            f"E2E-4 违反: error 日志缺源路径可定位信息\n  log={log_text!r}"
        )

    def test_upload_network_failure_is_observable(self, tmp_path: Path,
                                                   monkeypatch: pytest.MonkeyPatch,
                                                   caplog):
        """E2E-4 ②: bridge 端不可达时(网络失败),
        必须 logging.error 带 url + agent + session + err,不静默吞.
        """
        import logging as _logging
        from agentflow_runtime import skill_executor as se

        agent_dir = tmp_path / "test-agent"
        outputs_dir = agent_dir / "outputs"
        outputs_dir.mkdir(parents=True)
        artifact_file = outputs_dir / "look.png"
        artifact_file.write_bytes(b"\x89PNG\r\n\x1a\nfake")

        monkeypatch.setattr(se, "AGENT_PACKAGE_DIR", str(agent_dir))
        monkeypatch.setenv("AF_USER_ID", "user_abc")
        monkeypatch.setenv("AF_CLIENT_ID", "client_xyz")
        # 指向一个不存在的端口,触发 URLError(网络失败)
        monkeypatch.setenv("AF_ARTIFACT_STORAGE_URL", "http://127.0.0.1:1")

        with caplog.at_level(_logging.ERROR, logger="root"):
            se._upload_artifacts_to_storage([str(artifact_file)])

        # E2E-4 断言:必须有 error 级日志,且含 url + err 可定位信息
        error_records = [r for r in caplog.records if r.levelno == _logging.ERROR]
        assert error_records, (
            "E2E-4 违反: upload.failed 无 error 级日志,失败被静默吞\n"
            "  必须有 logging.error 带 url + agent + session + err"
        )
        log_text = " ".join(r.getMessage() for r in error_records)
        assert "artifact.upload.failed" in log_text or "upload.failed" in log_text, (
            f"E2E-4 违反: error 日志缺 upload.failed 标记\n  log={log_text!r}"
        )
        assert "127.0.0.1:1" in log_text, (
            f"E2E-4 违反: error 日志缺目标 URL 可定位信息\n  log={log_text!r}"
        )

    def test_upload_missing_file_is_warning_not_silent(self, tmp_path: Path,
                                                        monkeypatch: pytest.MonkeyPatch,
                                                        caplog):
        """E2E-4 ③: 文件不存在时走 warning(skip_missing),不静默吞.

        设计文档明示缺失文件是 info/warning 级(正常跳过),非 error.
        但仍必须有日志——不静默吞是 E2E-4 的底线.
        """
        import logging as _logging
        from agentflow_runtime import skill_executor as se

        agent_dir = tmp_path / "test-agent"
        outputs_dir = agent_dir / "outputs"
        outputs_dir.mkdir(parents=True)
        # 文件不存在
        missing_file = outputs_dir / "ghost.png"

        monkeypatch.setattr(se, "AGENT_PACKAGE_DIR", str(agent_dir))
        monkeypatch.setenv("AF_USER_ID", "user_abc")
        monkeypatch.setenv("AF_CLIENT_ID", "client_xyz")
        monkeypatch.setenv("AF_ARTIFACT_STORAGE_URL", "http://localhost:8199")

        with caplog.at_level(_logging.WARNING, logger="root"):
            se._upload_artifacts_to_storage([str(missing_file)])

        # E2E-4 断言:必须有 warning 级日志(不静默吞),含 path
        warning_records = [r for r in caplog.records if r.levelno >= _logging.WARNING]
        assert warning_records, (
            "E2E-4 违反: skip_missing 无任何 warning 级日志,缺失文件被静默吞"
        )
        log_text = " ".join(r.getMessage() for r in warning_records)
        assert "ghost.png" in log_text or str(missing_file) in log_text, (
            f"E2E-4 违反: warning 日志缺源路径可定位信息\n  log={log_text!r}"
        )


# ═══════════════════════════════════════════════════════════════════
#  E2E-3 不变量测试:URL 会话段与上传落盘目录段一致性
# ═══════════════════════════════════════════════════════════════════


class TestArtifactSessionSegmentConsistency:
    """E2E-3 锁死: artifact_id 的会话段必须单一真相来源.

    设计文档输出侧方法论 §11.4 不变量 E2E-3:
    - URL 里的 {user_id}_{client_id} 段必须与上传落盘目录段一致
    - 不得一个走 AF_SESSION_ID(agentflow 执行槽位 0/1/2)、一个走 {user_id}_{client_id}

    已知裂缝(2026-08-12 查证):
    - local_artifact_url(coordinator_decide.py:592)读 AF_SESSION_ID
    - _upload_artifacts_to_storage(skill_executor.py:936)调 _artifact_session_id()
      拼 {user_id}_{client_id}(runtime_context.py:64-75)
    - 两段来源不一致,靠 Bridge 剥离会话段重试兜底,非终态

    本测试类坐实裂缝存在,修复后应通过.
    """

    def test_url_segment_matches_upload_segment(self, tmp_path: Path,
                                                 monkeypatch: pytest.MonkeyPatch):
        """E2E-3 核心断言: local_artifact_url 生成的 URL 会话段
        必须与 _upload_artifacts_to_storage 落盘的会话段一致.

        场景:同一 agent 同一会话产出文件,两端必须拼到同一 artifact_id.
        """
        from agentflow_runtime.common.runtime_context import artifact_session_id
        from agentflow_runtime.coordinator_common import local_artifact_url

        agent_dir = tmp_path / "test-agent"
        outputs_dir = agent_dir / "outputs"
        outputs_dir.mkdir(parents=True)
        artifact_file = outputs_dir / "look.png"
        artifact_file.write_bytes(b"\x89PNG\r\n\x1a\nfake")

        monkeypatch.setattr("agentflow_runtime.coordinator_common.AGENT_PACKAGE_DIR",
                            str(agent_dir), raising=False)
        # 模拟真实注入:{user_id}_{client_id} + 执行槽位 同时存在
        monkeypatch.setenv("AF_USER_ID", "user_abc")
        monkeypatch.setenv("AF_CLIENT_ID", "client_xyz")
        monkeypatch.setenv("AF_SESSION_ID", "0")  # agentflow 执行槽位
        monkeypatch.setenv("AF_ARTIFACT_STORAGE_URL", "http://localhost:8199")

        # 上传侧落盘会话段与产物 URL 共用 runtime_context 单一真相源。
        upload_segment = artifact_session_id()
        expected_segment = "user_abc_client_xyz"  # {user_id}_{client_id}
        assert upload_segment == expected_segment, (
            f"上传侧会话段来源异常. expected={expected_segment} got={upload_segment}"
        )

        # URL 侧生成的会话段(local_artifact_url)
        url = local_artifact_url(str(artifact_file), str(agent_dir))
        # E2E-3 核心断言:URL 的会话段必须与上传侧一致
        # URL 形如 /api/artifacts/{agent}/{segment}/outputs/look.png
        url_parts = url.strip("/").split("/")
        # 形如 ['api', 'artifacts', 'test-agent', '{segment}', 'outputs', 'look.png']
        if len(url_parts) >= 4 and url_parts[0] == "api" and url_parts[1] == "artifacts":
            url_segment = url_parts[3]
            assert url_segment == expected_segment, (
                f"E2E-3 违反: URL 会话段与上传落盘会话段不一致.\n"
                f"  URL 侧(local_artifact_url): {url_segment} (来源 AF_SESSION_ID='0')\n"
                f"  上传侧(_upload_artifacts_to_storage): {expected_segment} "
                f"(来源 AF_USER_ID + AF_CLIENT_ID)\n"
                f"  URL={url}\n"
                f"  根因: local_artifact_url 读 AF_SESSION_ID(执行槽位),"
                f" 上传侧读 {{user_id}}_{{client_id}},两段来源不一致\n"
                f"  修复: local_artifact_url 应改读 _artifact_session_id()"
            )
        else:
            # 走了 legacy 分支(无 session_id 段)——这本身就是问题
            if "files" in url:
                raise AssertionError(
                    f"E2E-3 违反: URL 走 legacy 分支(无会话段),"
                    f"但上传侧有会话段 {expected_segment}.\n"
                    f"  URL={url}\n"
                    f"  根因: local_artifact_url 的 session_id=AF_SESSION_ID 走"
                    f" legacy 分支,但 _upload_artifacts_to_storage 走 {user_id}_{client_id}"
                )


# ═══════════════════════════════════════════════════════════════════
#  E2E-5 不变量测试:对外 URL 替换不许写宿主机绝对路径
# ═══════════════════════════════════════════════════════════════════


class TestRewriteLocalArtifactLinksNoHostPath:
    """E2E-5 锁死: 对外 URL 替换不许写宿主机绝对路径.

    设计文档输出侧方法论 §11.4 不变量 E2E-5(用户 2026-08-12 明确):
    - 两种路径替换语义边界永不混:
      ① 对外访问 URL 替换(写进对话文本,给客户端浏览器用,形态 /api/artifacts/...)
      ② 沙箱内→宿主机路径(环节①内部记账,永不写进对话文本)
    - 违反场景:把宿主机绝对路径写进对话文本 → 下一轮 agent 拿宿主机路径在沙箱里
      open(path) 必然找不到 → 对话断链

    当前实现未违反(rewrite_local_artifact_links 返回 local_artifact_url 的 serve URL),
    本测试锁死防回归.
    """

    def test_rewrite_no_host_absolute_path_in_output(self, tmp_path: Path,
                                                      monkeypatch: pytest.MonkeyPatch):
        """E2E-5 核心断言: rewrite_local_artifact_links 改写后的文本
        不许出现宿主机绝对路径,只允许 /api/artifacts/... serve URL.

        场景:LLM 输出含两条引用——包内产物 + 包外宿主机绝对路径产物.
        两条都应被改写为 serve URL,文本中不许残留任何宿主机绝对路径.
        """
        from agentflow_runtime.coordinator_common import rewrite_local_artifact_links
        import agentflow_runtime.coordinator_common as cc

        agent_dir = tmp_path / "test-agent"
        outputs_dir = agent_dir / "outputs"
        outputs_dir.mkdir(parents=True)

        # 产物①:包内产物(常规场景)
        in_pkg_artifact = outputs_dir / "look.png"
        in_pkg_artifact.write_bytes(b"\x89PNG\r\n\x1a\nfake-in-pkg")

        # 产物②:包外宿主机绝对路径产物(极端场景:LLM 引用了 /tmp 或 /Users 下的文件)
        external_dir = tmp_path / "external"
        external_dir.mkdir()
        external_artifact = external_dir / "extra.png"
        external_artifact.write_bytes(b"\x89PNG\r\n\x1a\nfake-external")

        monkeypatch.setattr(cc, "AGENT_PACKAGE_DIR", str(agent_dir))
        monkeypatch.setenv("AF_ARTIFACT_STORAGE_URL", "http://localhost:8199")
        monkeypatch.setenv("AF_USER_ID", "user_abc")
        monkeypatch.setenv("AF_CLIENT_ID", "client_xyz")
        # 故意设 AF_SESSION_ID 证明即使有它也不该走宿主机路径
        monkeypatch.setenv("AF_SESSION_ID", "0")

        # LLM 输出:两条引用,一条相对路径一条绝对路径
        content = (
            f"![包内图](outputs/look.png)\n"
            f"![包外图]({external_artifact})\n"
        )

        result = rewrite_local_artifact_links(
            content, [str(in_pkg_artifact), str(external_artifact)]
        )

        # E2E-5 核心断言①:改写后文本不许出现宿主机绝对路径
        host_abs_markers = [str(tmp_path), "/tmp/", "/Users/", "/home/", "/var/"]
        for marker in host_abs_markers:
            assert marker not in result, (
                f"E2E-5 违反: 改写后文本残留宿主机绝对路径标记 '{marker}'.\n"
                f"  result={result!r}\n"
                f"  根因: rewrite_local_artifact_links 或 local_artifact_url 返回了"
                f"宿主机路径而非 serve URL"
            )

        # E2E-5 核心断言②:改写后文本必须含 serve URL(证明替换确实发生了)
        assert "/api/artifacts/" in result, (
            f"E2E-5 辅助断言失败: 改写后文本未含 serve URL,替换可能未发生.\n"
            f"  result={result!r}"
        )

    def test_local_artifact_url_returns_serve_url_not_host_path(self, tmp_path: Path,
                                                                monkeypatch: pytest.MonkeyPatch):
        """E2E-5 单元断言: local_artifact_url 返回的必须是 /api/artifacts/... serve URL,
        不许是宿主机绝对路径.

        这是最窄的防回归断言——直接锁 local_artifact_url 的输出形态.
        """
        from agentflow_runtime.coordinator_common import local_artifact_url
        import agentflow_runtime.coordinator_common as cc

        agent_dir = tmp_path / "test-agent"
        outputs_dir = agent_dir / "outputs"
        outputs_dir.mkdir(parents=True)
        artifact_file = outputs_dir / "look.png"
        artifact_file.write_bytes(b"\x89PNG\r\n\x1a\nfake")

        monkeypatch.setattr(cc, "AGENT_PACKAGE_DIR", str(agent_dir))
        monkeypatch.setenv("AF_ARTIFACT_STORAGE_URL", "http://localhost:8199")
        monkeypatch.setenv("AF_USER_ID", "user_abc")
        monkeypatch.setenv("AF_CLIENT_ID", "client_xyz")
        monkeypatch.setenv("AF_SESSION_ID", "0")

        url = local_artifact_url(str(artifact_file), str(agent_dir))

        # E2E-5 形态断言:必须以 /api/artifacts/ 开头(serve URL)
        assert url.startswith("/api/artifacts/"), (
            f"E2E-5 违反: local_artifact_url 返回的不是 serve URL.\n"
            f"  url={url!r}\n"
            f"  必须以 '/api/artifacts/' 开头"
        )
        # E2E-5 不许含宿主机绝对路径标记
        host_abs_markers = [str(tmp_path), "/tmp/", "/Users/", "/home/", "/var/"]
        for marker in host_abs_markers:
            assert marker not in url, (
                f"E2E-5 违反: local_artifact_url 返回值含宿主机路径标记 '{marker}'.\n"
                f"  url={url!r}"
            )


# ═══════════════════════════════════════════════════════════════════
#  沙箱产物下载测试（TDD Red）
# ═══════════════════════════════════════════════════════════════════


class TestSandboxArtifactDownload:
    """环节①: 沙箱内生成的产物文件应能下载到宿主机。

    设计文档输出侧方法论 §11.4 端到端闭环环节①:
    - E2E-1: 沙箱内产物必须能拉回宿主机(Skill 不感知沙箱 vs 宿主机差异)
    - E2E-2: 二进制安全贯穿全链(音视频/PDF/图片不得损坏)

    双通道:
    - Local/Docker 通道(_download_from_sandbox): POST {sandbox_url}/artifact/download
      返回 base64 编码的原始字节,不经过 read_file 文本解码或字符数截断
    - e2b 通道(_download_from_sandbox_e2b): e2b SDK envd /files 端点,二进制安全

    env var 契约(2026-08-12 校正):
    - AF_SANDBOX_ENDPOINT: 沙箱 HTTP server 地址,_ensure_artifacts_in_package 拉回用
    - AF_ARTIFACT_STORAGE_URL: bridge 存储中心地址,_upload_artifacts_to_storage 转存用
      两者独立,曾错配导致测试驱动不起实现分支
    """

    def test_download_missing_file_from_sandbox_local_docker(self, tmp_path: Path,
                                                              monkeypatch: pytest.MonkeyPatch):
        """Local/Docker 通道: 宿主机不存在的文件应从沙箱 HTTP server 下载。

        驱动 _ensure_artifacts_in_package → _download_from_sandbox 路径。
        env var 必须设 AF_SANDBOX_ENDPOINT(非 AF_ARTIFACT_STORAGE_URL)。
        """
        import base64, threading, http.server, json
        from agentflow_runtime import skill_executor as se

        sandbox_file_rel = "outputs/test-artifact.png"
        sandbox_content = b"fake-sandbox-file-content"

        class MockSandboxHandler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                if self.path == "/artifact/download":
                    length = int(self.headers.get("Content-Length", 0))
                    body = json.loads(self.rfile.read(length).decode() if length > 0 else "{}")
                    if body.get("path") == sandbox_file_rel:
                        resp = json.dumps({
                            "path": sandbox_file_rel,
                            "size": len(sandbox_content),
                            "content_b64": base64.b64encode(sandbox_content).decode("ascii"),
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
        threading.Thread(target=server.serve_forever, daemon=True).start()
        sandbox_url = f"http://127.0.0.1:{port}"

        agent_dir = tmp_path / "test-agent"
        agent_dir.mkdir()
        monkeypatch.setattr(se, "AGENT_PACKAGE_DIR", str(agent_dir))
        # env var 契约校正:拉回读 AF_SANDBOX_ENDPOINT,不是 AF_ARTIFACT_STORAGE_URL
        monkeypatch.setenv("AF_SANDBOX_ENDPOINT", sandbox_url)
        monkeypatch.setenv("AF_SANDBOX_MODE", "docker")

        try:
            abs_path = str(agent_dir / sandbox_file_rel)
            se._ensure_artifacts_in_package([abs_path])

            downloaded = agent_dir / sandbox_file_rel
            assert downloaded.is_file(), (
                f"文件未从沙箱下载到宿主机.\n"
                f"  sandbox_url={sandbox_url}\n  expected={downloaded}\n"
                f"  检查 _ensure_artifacts_in_package 是否读 AF_SANDBOX_ENDPOINT"
            )
            assert downloaded.read_bytes() == sandbox_content, "下载的文件内容不匹配"
        finally:
            server.shutdown()

    def test_download_binary_safe_local_docker(self, tmp_path: Path,
                                                monkeypatch: pytest.MonkeyPatch):
        """E2E-2 防回归: Local/Docker 通道拉回二进制内容不损坏.

        Local/Docker 必须返回 base64 原始字节，不能经过 read_file 的 UTF-8
        文本解码、10000 字符上限或 latin-1 逆编码。
        """
        import base64, threading, http.server, json
        from agentflow_runtime import skill_executor as se

        sandbox_file_rel = "outputs/binary-artifact.png"
        # 真 PNG 头 + 含非 latin-1 字节(0xFF 0x89 等高位字节)
        binary_content = b"\x89PNG\r\n\x1a\n" + bytes(range(128, 256)) + b"\xff\xfe\xfd"

        class MockSandboxHandler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                if self.path == "/artifact/download":
                    length = int(self.headers.get("Content-Length", 0))
                    body = json.loads(self.rfile.read(length).decode() if length > 0 else "{}")
                    if body.get("path") == sandbox_file_rel:
                        resp = json.dumps({
                            "path": sandbox_file_rel,
                            "size": len(binary_content),
                            "content_b64": base64.b64encode(binary_content).decode("ascii"),
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
        threading.Thread(target=server.serve_forever, daemon=True).start()

        agent_dir = tmp_path / "test-agent"
        agent_dir.mkdir()
        monkeypatch.setattr(se, "AGENT_PACKAGE_DIR", str(agent_dir))
        monkeypatch.setenv("AF_SANDBOX_ENDPOINT", f"http://127.0.0.1:{port}")
        monkeypatch.setenv("AF_SANDBOX_MODE", "docker")

        try:
            abs_path = str(agent_dir / sandbox_file_rel)
            se._ensure_artifacts_in_package([abs_path])

            downloaded = agent_dir / sandbox_file_rel
            assert downloaded.is_file(), "文件未从沙箱下载"
            # E2E-2 核心断言:二进制内容逐字节一致
            actual = downloaded.read_bytes()
            assert actual == binary_content, (
                f"E2E-2 违反: 二进制内容损坏.\n"
                f"  expected len={len(binary_content)} first={binary_content[:16]!r}\n"
                f"  actual   len={len(actual)} first={actual[:16]!r}\n"
                f"  根因: /artifact/download 未保留 base64 原始字节"
            )
        finally:
            server.shutdown()

    def test_download_missing_file_from_sandbox_e2b(self, tmp_path: Path,
                                                     monkeypatch: pytest.MonkeyPatch):
        """e2b 通道: 沙箱内产物用 e2b SDK envd /files 端点拉回,二进制安全.

        驱动 _ensure_artifacts_in_package → _download_from_sandbox_e2b 路径.
        mock E2BExecutor.download_to_host 避免依赖真实 e2b 服务,但走真实调用链
        (create_executor + download_to_host 接口契约),锁 E2E-1.

        macOS 路径陷阱:Path("/home/user/...").resolve() 会被绕到
        /System/Volumes/Data/home/user/...,导致 str(p).startswith(ws_prefix) 不过,
        误走 Local/Docker 通道.用 monkeypatch 锁 Path.resolve 返回原路径.
        """
        from unittest.mock import patch, MagicMock
        from pathlib import Path as _Path
        from agentflow_runtime import skill_executor as se

        e2b_ws_prefix = "/home/user/workspace"
        sandbox_file_rel = "outputs/e2b-artifact.png"
        binary_content = b"\x89PNG\r\n\x1a\n" + bytes(range(256)) + b"\xff\xfe\xfd"
        sandbox_abs_path = f"{e2b_ws_prefix}/{sandbox_file_rel}"

        agent_dir = tmp_path / "test-agent"
        agent_dir.mkdir()
        monkeypatch.setattr(se, "AGENT_PACKAGE_DIR", str(agent_dir))
        monkeypatch.setenv("AF_SANDBOX_ENDPOINT", "http://fake-e2b-endpoint")
        monkeypatch.setenv("AF_SANDBOX_MODE", "e2b")
        monkeypatch.setenv("AF_SANDBOX_WORKSPACE", e2b_ws_prefix)

        # macOS 路径陷阱防御:锁 Path.resolve 对 e2b 路径不重写
        _orig_resolve = _Path.resolve

        def _locked_resolve(self_path, *args, **kwargs):
            if str(self_path).startswith(e2b_ws_prefix):
                return self_path  # e2b 沙箱路径不重写
            return _orig_resolve(self_path, *args, **kwargs)
        monkeypatch.setattr(_Path, "resolve", _locked_resolve)

        downloaded_path = agent_dir / "outputs" / "e2b-artifact.png"

        def fake_download_to_host(sandbox_path, local_path):
            """模拟 e2b SDK download_to_host: 写入二进制内容到宿主机路径."""
            _Path(local_path).parent.mkdir(parents=True, exist_ok=True)
            _Path(local_path).write_bytes(binary_content)
            return True

        with patch("agentflow_runtime.executors.factory.create_executor") as mock_factory:
            mock_executor = MagicMock()
            mock_executor.download_to_host = fake_download_to_host
            mock_factory.return_value = mock_executor

            abs_path = sandbox_abs_path  # e2b 分支要求 abs_path startswith ws_prefix
            se._ensure_artifacts_in_package([abs_path])

            assert downloaded_path.is_file(), (
                f"e2b 通道未拉回文件.\n  expected={downloaded_path}\n"
                f"  检查 _download_from_sandbox_e2b 是否被正确触发"
            )
            # E2E-2: e2b 通道二进制安全断言
            actual = downloaded_path.read_bytes()
            assert actual == binary_content, (
                f"E2E-2 违反(e2b): 二进制内容损坏.\n"
                f"  expected len={len(binary_content)}\n  actual len={len(actual)}"
            )