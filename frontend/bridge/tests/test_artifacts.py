"""Integration tests for Bridge artifact delivery and containment."""

from __future__ import annotations

from pathlib import Path

import pytest
from fastapi import FastAPI
from fastapi.testclient import TestClient

from routes import artifacts


JPEG_BYTES = b"\xff\xd8\xff\xe0" + b"fixture-jpeg-content"


@pytest.fixture
def artifact_root(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    root = tmp_path / "packages"
    root.mkdir()
    monkeypatch.setattr(artifacts.settings, "agent_packages_dir", str(root))
    return root


@pytest.fixture
def client(artifact_root: Path) -> TestClient:
    app = FastAPI()
    app.include_router(artifacts.router)
    return TestClient(app)


def test_image_is_fetchable_with_real_mime(client: TestClient, artifact_root: Path) -> None:
    """TC-brg-011: 图片按真实字节返回，并使用真实 MIME。

    业务规则:
    - 包根目录内的图片必须可被外部客户端完整获取。
    - 图片扩展名错误时仍应按文件内容返回正确 MIME。

    错误模型:
    - 文件存在但路由不可访问；按扩展名返回错误 MIME；传输改变文件字节。

    失败意味着:
    - 聊天中虽然返回图片链接，浏览器仍可能无法显示或得到损坏文件。
    """
    image = artifact_root / "outfit_assistant" / "outputs" / "look.png"
    image.parent.mkdir(parents=True)
    image.write_bytes(JPEG_BYTES)

    response = client.get(
        "/api/artifacts/outfit-assistant/files/"
        "outfit_assistant/outputs/look.png"
    )

    assert response.status_code == 200
    assert response.content == JPEG_BYTES
    assert response.headers["content-type"] == "image/jpeg"


def test_html_relative_resource_tree_is_fetchable(
    client: TestClient, artifact_root: Path
) -> None:
    """TC-brg-012: HTML 与相对图片资源保持同一可访问目录树。

    业务规则:
    - HTML 中的相对资源引用必须能相对于 HTML URL 正常解析。

    错误模型:
    - 只发布 HTML 文件或扁平化产物目录，导致相对图片 404。

    失败意味着:
    - 外部服务能打开 HTML，但报告中的图片或样式缺失。
    """
    report_dir = artifact_root / "outfit_assistant" / "outputs" / "task-1"
    assets = report_dir / "assets"
    assets.mkdir(parents=True)
    (report_dir / "report.html").write_text(
        '<html><img src="assets/look.png"></html>', encoding="utf-8"
    )
    (assets / "look.png").write_bytes(JPEG_BYTES)
    (assets / "theme.css").write_text("img { max-width: 100%; }", encoding="utf-8")

    base = "/api/artifacts/outfit-assistant/files/outfit_assistant/outputs/task-1"
    html = client.get(f"{base}/report.html")
    image = client.get(f"{base}/assets/look.png")
    css = client.get(f"{base}/assets/theme.css")

    assert html.status_code == 200
    assert html.headers["content-type"].startswith("text/html")
    assert 'src="assets/look.png"' in html.text
    assert image.status_code == 200
    assert image.content == JPEG_BYTES
    assert image.headers["content-type"] == "image/jpeg"
    assert css.status_code == 200
    assert "max-width" in css.text


@pytest.mark.parametrize(
    "requested_path",
    [
        "../secret.png",
        "%2e%2e/secret.png",
        "outfit_assistant/../../secret.png",
    ],
)
def test_artifact_path_escape_is_rejected(
    client: TestClient,
    artifact_root: Path,
    requested_path: str,
) -> None:
    """TC-brg-014: 目录穿越不能读取 Agent Package 根目录外文件。

    业务规则:
    - Artifact 服务只能读取配置的 Agent Package 根目录内文件。

    错误模型:
    - 路径规范化缺失或发生在校验之后，允许 ``..`` 逃逸。

    失败意味着:
    - 外部调用方可能读取宿主机敏感文件。
    """
    secret = artifact_root.parent / "secret.png"
    secret.write_bytes(JPEG_BYTES)

    response = client.get(
        f"/api/artifacts/outfit-assistant/files/{requested_path}"
    )

    assert response.status_code in {403, 404}
    assert response.content != JPEG_BYTES


def test_artifact_symlink_escape_is_rejected(
    client: TestClient, artifact_root: Path
) -> None:
    """TC-brg-014: 指向包根目录外的符号链接不能绕过路径边界。

    业务规则:
    - 路径边界以解析后的真实路径为准，符号链接不得扩大读取范围。

    错误模型:
    - 仅对原始字符串做前缀检查，未解析 symlink。

    失败意味着:
    - 攻击者可在包内放置 symlink 读取任意宿主机文件。
    """
    outside = artifact_root.parent / "outside.png"
    outside.write_bytes(JPEG_BYTES)
    link = artifact_root / "outfit_assistant" / "outputs" / "outside.png"
    link.parent.mkdir(parents=True)
    try:
        link.symlink_to(outside)
    except OSError as exc:
        pytest.skip(f"symlink not supported in this environment: {exc}")

    response = client.get(
        "/api/artifacts/outfit-assistant/files/"
        "outfit_assistant/outputs/outside.png"
    )

    assert response.status_code == 403
    assert response.content != JPEG_BYTES


def test_unsupported_artifact_type_is_rejected(
    client: TestClient, artifact_root: Path
) -> None:
    """TC-brg-014: 包根目录内的非白名单文件也不能被下载。

    业务规则:
    - 路径合法与文件类型合法必须同时满足。

    错误模型:
    - 只检查路径包含关系，遗漏文件类型白名单。

    失败意味着:
    - Artifact API 退化为 Agent Package 任意文件下载接口。
    """
    source = artifact_root / "outfit_assistant" / "agent.py"
    source.parent.mkdir(parents=True)
    source.write_text("API_KEY = 'secret'", encoding="utf-8")

    response = client.get(
        "/api/artifacts/outfit-assistant/files/outfit_assistant/agent.py"
    )

    assert response.status_code == 415
    assert b"API_KEY" not in response.content
