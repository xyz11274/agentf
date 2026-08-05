"""真实链路 E2E:大图上传 → bridge 存储原图 → 读取压缩(artifact-storage.md §2.4)。

覆盖:artifact_payload=base64 时 SSE 内嵌路径(read_artifact_base64 → _encode_file)
压缩到最长边 ≤512px;开关关闭返回 raw base64;存储层不被污染(原图保留)。
"""

from __future__ import annotations

import base64
import io
from pathlib import Path

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient
from PIL import Image

from models import ArtifactInfo
from routes import artifacts
from routes.artifact_extractor import apply_artifact_output_mode
from routes.artifacts import read_artifact_base64


@pytest.fixture
def bridge_app(tmp_path: Path) -> FastAPI:
    """真实 bridge FastAPI 应用,存储目录指向临时目录。"""
    app = FastAPI()
    app.include_router(artifacts.router)
    artifacts._ARTIFACTS_STORAGE_DIR = tmp_path / "artifacts_storage"
    return app


@pytest.fixture
def client(bridge_app: FastAPI) -> TestClient:
    return TestClient(bridge_app)


def _make_large_png(size: int = 4096, noise_step: int = 32) -> bytes:
    """生成一张 size×size 带噪点的大 PNG(模拟 skill 生成的 1.4MB 级图片)。"""
    img = Image.new("RGB", (size, size), (120, 80, 200))
    for x in range(0, size, noise_step):
        for y in range(0, size, noise_step):
            img.putpixel((x, y), ((x * 7) % 256, (y * 5) % 256, 128))
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return buf.getvalue()


def _upload(client: TestClient, raw: bytes, path: str = "outputs/look.png") -> str:
    """上传大图到 bridge 存储中心,返回 artifact_id。"""
    resp = client.post("/api/artifacts/upload", json={
        "agent_name": "outfit-assistant",
        "session_id": "user_e2e_client_e2e",
        "files": [{"path": path, "data_b64": base64.b64encode(raw).decode("ascii")}],
    })
    assert resp.status_code == 200, f"upload failed: {resp.status_code} {resp.text}"
    ids = resp.json()["artifact_ids"]
    assert len(ids) == 1
    return ids[0]


class TestArtifactCompressE2E:
    """真实链路:上传大图 → 存储原图 → 读取压缩到 512px。"""

    def test_upload_stores_original_read_compresses(
        self, client: TestClient, tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
    ) -> None:
        """上传 4096px 大图:存储层保留原图,读取返回压缩 base64(≤512px, 体积大幅减小)。"""
        monkeypatch.setattr(artifacts.settings, "artifact_compress_enabled", True)

        raw = _make_large_png(4096, 32)
        artifact_id = _upload(client, raw)
        print(f"[e2e] 原始图片: {len(raw)/1024:.0f} KB")

        # 存储层不被污染:落盘文件 == 上传原图
        stored = tmp_path / "artifacts_storage" / artifact_id
        assert stored.is_file()
        assert stored.read_bytes() == raw, "存储层应保留原始文件(不污染)"

        # 真实读取链路(SSE 内嵌 base64 同路径):read_artifact_base64 → _encode_file 压缩
        url = f"/api/artifacts/{artifact_id}"
        b64 = read_artifact_base64(url, "image/png")
        assert b64, "read_artifact_base64 应返回非空 base64"
        data = base64.b64decode(b64)
        img = Image.open(io.BytesIO(data))
        longest = max(img.width, img.height)
        print(f"[e2e] 压缩后: {len(data)/1024:.0f} KB, 尺寸 {img.width}x{img.height}, 最长边 {longest}px")
        print(f"[e2e] 压缩比: {len(raw)/len(data):.1f}x")

        assert longest <= 512, f"压缩后最长边应 ≤512,got {longest}"
        assert len(data) < len(raw), "压缩后体积应小于原图"

    def test_compress_switch_off_returns_raw(
        self, client: TestClient, tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
    ) -> None:
        """开关关闭:读取返回 raw base64(不压缩),存储原图不变。"""
        raw = _make_large_png(2048, 64)
        artifact_id = _upload(client, raw)

        monkeypatch.setattr(artifacts.settings, "artifact_compress_enabled", False)
        b64 = read_artifact_base64(f"/api/artifacts/{artifact_id}", "image/png")
        assert base64.b64decode(b64) == raw, "开关关闭应返回 raw base64(逐字节相等)"

    def test_sse_payload_base64_path_applies_compression(
        self, client: TestClient, tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
    ) -> None:
        """SSE 内嵌路径(artifact_payload=base64)走 apply_artifact_output_mode → 压缩生效。"""
        monkeypatch.setattr(artifacts.settings, "artifact_compress_enabled", True)
        monkeypatch.setattr(artifacts.settings, "artifact_reference", "link")
        monkeypatch.setattr(artifacts.settings, "artifact_payload", "base64")

        raw = _make_large_png(2048, 64)
        artifact_id = _upload(client, raw)
        url = f"/api/artifacts/{artifact_id}"

        art = ArtifactInfo(type="image", url=url, name="look.png", mime_type="image/png")
        content = "结果：![look](%s)" % url
        new_content, artifacts_out = apply_artifact_output_mode(content, [art])
        assert artifacts_out[0].data_base64, "base64 内嵌模式应填充 data_base64"
        data = base64.b64decode(artifacts_out[0].data_base64)
        img = Image.open(io.BytesIO(data))
        longest = max(img.width, img.height)
        print(f"[e2e] SSE 内嵌: {len(data)/1024:.0f} KB, 最长边 {longest}px")
        assert longest <= 512, f"SSE 内嵌 base64 最长边应 ≤512,got {longest}"
        assert "data:image/png;base64," in new_content, "link+base64 应生成 data URI"
