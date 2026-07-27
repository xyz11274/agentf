"""pytest 骨架——Bridge sandbox/files 端点双通道回退测试。

覆盖 AC-US4-002，引用 TC-SBOX-030/062。
用 httpx TestClient 测 Bridge 端点，用内存 ArtifactStore 替代真实快照。
"""
import json
import tempfile
import threading
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest
from fastapi.testclient import TestClient

# 导入 Bridge 应用
from main import app
from routes.sandbox_files import set_artifact_store


class _InMemoryArtifactStore:
    """内存版 ArtifactStore（替代真实 C++ 实现，供测试用）。"""
    def __init__(self):
        self.snapshots: dict[str, dict[str, str]] = {}  # sandbox_id -> {path: content}

    def has_snapshot(self, sandbox_id: str) -> bool:
        return sandbox_id in self.snapshots

    def read_snapshot(self, sandbox_id: str, path: str) -> list[str]:
        store = self.snapshots.get(sandbox_id, {})
        content = store.get(path)
        if content is None:
            return []
        return content.split("\n")


class _MockSandboxServer(BaseHTTPRequestHandler):
    """模拟沙箱内 HTTP server（用于运行时通道测试）。"""
    def do_GET(self):
        if self.path.startswith("/files?path="):
            file_path = self.path.split("=", 1)[1]
            if file_path == "result.png":
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.end_headers()
                self.wfile.write(b"mock-image-data")
            elif file_path == "hello.txt":
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(b"hello from sandbox")
            else:
                self.send_response(404)
                self.end_headers()
                self.wfile.write(b"Not found")
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, format, *args):
        pass  # 静默日志


@pytest.fixture
def mock_sandbox_server():
    """启动一个本地 HTTP server 模拟沙箱内 HTTP server。"""
    server = ThreadingHTTPServer(("127.0.0.1", 0), _MockSandboxServer)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield f"http://127.0.0.1:{port}"
    server.shutdown()


@pytest.fixture
def client():
    """TestClient 实例，注入内存 ArtifactStore。"""
    store = _InMemoryArtifactStore()
    set_artifact_store(store)
    with TestClient(app) as c:
        c._store = store  # 挂载到 client 上，供测试用例读写
        yield c


# Bridge 挂载在 /health 下（main.py: app.mount("/health", bridge_app)）
_PREFIX = "/health/v1/sessions"


# TC-SBOX-030: 销毁后前端 GET 产物回退读快照不死链
def test_destroyed_reads_snapshot(client):
    """TC-SBOX-030: 销毁后前端 GET 产物回退读快照不死链。"""
    store = client._store
    store.snapshots["sb-001"] = {"result.png": "image binary data here"}

    resp = client.get(f"{_PREFIX}/sb-001/sandbox/files?path=result.png")
    assert resp.status_code == 200, f"应为 200: {resp.status_code} {resp.text}"
    data = resp.json()
    assert "content" in data, f"响应缺 content 字段: {data}"
    assert data["content"] == "image binary data here", f"内容不一致: {data}"
    assert data["source"] == "snapshot", f"source 应=snapshot: {data}"


# TC-SBOX-062: 契约测试——Bridge 期望存活转发、销毁读快照、全不可访 410
def test_contract_dual_channel(client):
    """TC-SBOX-062: 契约测试——Bridge 期望沙箱存活转发、销毁后读快照。"""
    store = client._store

    # 1. 销毁有快照 → 200（快照通道）
    store.snapshots["sb-002"] = {"hello.txt": "hello world"}
    resp = client.get(f"{_PREFIX}/sb-002/sandbox/files?path=hello.txt")
    assert resp.status_code == 200, f"有快照应 200: {resp.status_code} {resp.text}"
    data = resp.json()
    assert data["content"] == "hello world"
    assert data["source"] == "snapshot"

    # 2. 销毁无快照 → 410
    resp = client.get(f"{_PREFIX}/nonexistent/sandbox/files?path=missing.txt")
    assert resp.status_code == 410, f"无快照应 410: {resp.status_code} {resp.text}"
    data = resp.json()
    assert "SANDBOX_AND_SNAPSHOT_GONE" in str(data), f"410 应含错误码: {data}"

    # 3. 有快照但文件不存在 → 404
    resp = client.get(f"{_PREFIX}/sb-002/sandbox/files?path=nonexistent.txt")
    assert resp.status_code == 404, f"文件不存在应 404: {resp.status_code} {resp.text}"
    data = resp.json()
    assert "FILE_NOT_FOUND" in str(data), f"404 应含错误码: {data}"


# 运行时通道：沙箱存活时转发到沙箱内 HTTP server
def test_runtime_channel_forwarding(client, monkeypatch):
    """TC-SBOX-062: 运行时通道——沙箱存活时 daemon 返回文件内容。
    
    模拟 daemon RPC 返回文件内容，验证 Bridge 直接返回。
    """
    # patch 现有 daemon_client 的 send_request 方法
    dc = app.state.daemon_client
    async def mock_read(method: str, params: dict = None):
        assert method == "sandbox.read_file"
        assert params["sandbox_id"] == "live-sb"
        if params["path"] == "hello.txt":
            return {"found": True, "content": "hello from sandbox", "source": "runtime"}
        return {"found": False}
    monkeypatch.setattr(dc, "send_request", mock_read)

    # 请求沙箱内文件，daemon 返回文件内容
    resp = client.get(f"{_PREFIX}/live-sb/sandbox/files?path=hello.txt")
    assert resp.status_code == 200, f"运行时通道应 200: {resp.status_code} {resp.text}"
    assert resp.text == "hello from sandbox", f"内容应为 daemon 返回: {resp.text}"

    # 请求不存在的文件 → daemon 返回 found=false，无快照 → 410
    resp = client.get(f"{_PREFIX}/live-sb/sandbox/files?path=nonexistent.png")
    assert resp.status_code == 410, f"文件不存在且无快照应 410: {resp.status_code} {resp.text}"


# 运行时通道 RPC 失败时回退到快照通道
def test_runtime_channel_fallback_to_snapshot(client, monkeypatch):
    """RPC 失败时自动回退到快照通道。"""
    dc = app.state.daemon_client
    async def mock_fail(method: str, params: dict = None):
        raise ConnectionError("daemon unreachable")
    monkeypatch.setattr(dc, "send_request", mock_fail)

    store = client._store
    store.snapshots["fallback-sb"] = {"data.txt": "fallback content"}

    resp = client.get(f"{_PREFIX}/fallback-sb/sandbox/files?path=data.txt")
    assert resp.status_code == 200, f"回退到快照应 200: {resp.status_code} {resp.text}"
    data = resp.json()
    assert data["content"] == "fallback content"
    assert data["source"] == "snapshot"


# 运行时通道：daemon 返回未找到时回退到快照通道
def test_runtime_channel_not_found_fallback(client, monkeypatch):
    """daemon 返回 found=false 时回退到快照通道。"""
    dc = app.state.daemon_client
    async def mock_not_found(method: str, params: dict = None):
        return {"found": False}
    monkeypatch.setattr(dc, "send_request", mock_not_found)

    store = client._store
    store.snapshots["dead-sb"] = {"report.txt": "archived report"}

    resp = client.get(f"{_PREFIX}/dead-sb/sandbox/files?path=report.txt")
    assert resp.status_code == 200, f"daemon 未找到但快照存在应 200: {resp.status_code} {resp.text}"
    data = resp.json()
    assert data["content"] == "archived report"
    assert data["source"] == "snapshot"


# 额外：空快照（快照存在但无文件）返回 404
def test_empty_snapshot_returns_404(client):
    """空快照（快照存在但无文件）返回 404 FILE_NOT_FOUND。"""
    store = client._store
    store.snapshots["empty-sb"] = {}
    resp = client.get(f"{_PREFIX}/empty-sb/sandbox/files?path=any.txt")
    assert resp.status_code == 404, f"空快照文件不存在应 404: {resp.status_code} {resp.text}"
    data = resp.json()
    assert "FILE_NOT_FOUND" in str(data), f"404 应含错误码: {data}"


# 额外：path 参数缺失返回 422
def test_missing_path_returns_422(client):
    """缺少 path 参数返回 422。"""
    resp = client.get(f"{_PREFIX}/sb-001/sandbox/files")
    assert resp.status_code == 422, f"缺 path 应 422: {resp.status_code} {resp.text}"


# 额外：多文件快照全部可读
def test_multi_file_snapshot(client):
    """多文件快照全部可读。"""
    store = client._store
    store.snapshots["multi"] = {
        "file1.txt": "content1",
        "sub/file2.txt": "content2",
    }
    resp1 = client.get(f"{_PREFIX}/multi/sandbox/files?path=file1.txt")
    assert resp1.status_code == 200
    assert resp1.json()["content"] == "content1"
    resp2 = client.get(f"{_PREFIX}/multi/sandbox/files?path=sub/file2.txt")
    assert resp2.status_code == 200
    assert resp2.json()["content"] == "content2"