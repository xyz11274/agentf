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
    # 三态模型：artifacts 读取根在 ③ workspace 实例（agent_workspace_path）
    root = tmp_path / "workspace"
    root.mkdir()
    monkeypatch.setattr(artifacts.settings, "agent_workspace_dir", str(root))
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
    image = artifact_root / "outfit-assistant" / "outputs" / "look.png"
    image.parent.mkdir(parents=True)
    image.write_bytes(JPEG_BYTES)

    response = client.get(
        "/api/artifacts/outfit-assistant/files/"
        "outputs/look.png"
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
    report_dir = artifact_root / "outfit-assistant" / "outputs" / "task-1"
    assets = report_dir / "assets"
    assets.mkdir(parents=True)
    (report_dir / "report.html").write_text(
        '<html><img src="assets/look.png"></html>', encoding="utf-8"
    )
    (assets / "look.png").write_bytes(JPEG_BYTES)
    (assets / "theme.css").write_text("img { max-width: 100%; }", encoding="utf-8")

    base = "/api/artifacts/outfit-assistant/files/outputs/task-1"
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
        "outfit-assistant/../../secret.png",
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
    link = artifact_root / "outfit-assistant" / "outputs" / "outside.png"
    link.parent.mkdir(parents=True)
    try:
        link.symlink_to(outside)
    except OSError as exc:
        pytest.skip(f"symlink not supported in this environment: {exc}")

    response = client.get(
        "/api/artifacts/outfit-assistant/files/"
        "outputs/outside.png"
    )

    assert response.status_code == 404
    assert response.content != JPEG_BYTES


# ═══════════════════════════════════════════════════════════════════
# Dual-path regression: /api/artifacts vs /health/api/artifacts
# ═══════════════════════════════════════════════════════════════════


@pytest.fixture
def dual_path_app(artifact_root: Path) -> FastAPI:
    """模拟 main.py 的双路径注册：外层 app 直挂 + bridge_app 挂 /health。

    main.py 中:
      app.include_router(artifacts_router)   # 外层直挂
      app.mount("/health", bridge_app)        # bridge_app 内也有 artifacts_router
    """
    bridge_app = FastAPI()
    bridge_app.include_router(artifacts.router)
    app = FastAPI()
    app.include_router(artifacts.router)
    app.mount("/health", bridge_app)
    return app


@pytest.fixture
def dual_path_client(dual_path_app: FastAPI) -> TestClient:
    return TestClient(dual_path_app)


_ARTIFACT_REL = "outfit-assistant/outputs/dual-path-test.png"


def test_outer_app_serves_without_health_prefix(
    dual_path_client: TestClient, artifact_root: Path
) -> None:
    """外层 app 直挂的 artifacts 路由应能通过 /api/artifacts/... 访问。

    回归目标：coordinator 生成的裸 /api/artifacts/... URL 在 Bridge 侧不被
    /health 前缀绑架。本地 dev 通过 Vite proxy /api → Bridge 时走此路径。
    """
    path = artifact_root / _ARTIFACT_REL
    path.parent.mkdir(parents=True)
    path.write_bytes(JPEG_BYTES)

    resp = dual_path_client.get(f"/api/artifacts/{_ARTIFACT_REL}")

    assert resp.status_code == 200, (
        f"Outer app should serve /api/artifacts/... (no /health), "
        f"got {resp.status_code}: {resp.text}"
    )
    assert resp.content == JPEG_BYTES


def test_inner_app_serves_with_health_prefix(
    dual_path_client: TestClient, artifact_root: Path
) -> None:
    """bridge_app 挂载在 /health 下时，/health/api/artifacts/... 也应可达。

    回归目标：生产环境网关加 /health 前缀后，Bridge 内部仍能解析。
    """
    path = artifact_root / _ARTIFACT_REL
    path.parent.mkdir(parents=True)
    path.write_bytes(JPEG_BYTES)

    resp = dual_path_client.get(f"/health/api/artifacts/{_ARTIFACT_REL}")

    assert resp.status_code == 200, (
        f"Inner app should serve /health/api/artifacts/... (with /health), "
        f"got {resp.status_code}: {resp.text}"
    )
    assert resp.content == JPEG_BYTES


def test_both_paths_return_same_content(
    dual_path_client: TestClient, artifact_root: Path
) -> None:
    """两条路径应返回完全相同的文件内容。"""
    path = artifact_root / _ARTIFACT_REL
    path.parent.mkdir(parents=True)
    path.write_bytes(JPEG_BYTES)

    without = dual_path_client.get(f"/api/artifacts/{_ARTIFACT_REL}")
    with_health = dual_path_client.get(f"/health/api/artifacts/{_ARTIFACT_REL}")

    assert without.status_code == 200
    assert with_health.status_code == 200
    assert without.content == with_health.content, (
        "Both paths should return identical file content"
    )


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
    source = artifact_root / "outfit-assistant" / "agent.py"
    source.parent.mkdir(parents=True)
    source.write_text("AF_LLM_API_KEY = 'secret'", encoding="utf-8")

    response = client.get(
        "/api/artifacts/outfit-assistant/files/agent.py"
    )

    assert response.status_code == 415
    assert b"AF_LLM_API_KEY" not in response.content


# ═══════════════════════════════════════════════════════════════════
# Non-image artifact delivery (PPT, DOCX, PDF, XLSX)
# ═══════════════════════════════════════════════════════════════════

def test_pptx_is_served(client: TestClient, artifact_root: Path) -> None:
    """Bridge 应能服务 .pptx 文件下载。"""
    pptx = artifact_root / "outfit-assistant" / "outputs" / "presentation.pptx"
    pptx.parent.mkdir(parents=True)
    pptx.write_bytes(b"PK\x03\x04\x00" + b"fake-pptx-content")

    response = client.get(
        "/api/artifacts/outfit-assistant/files/"
        "outputs/presentation.pptx"
    )

    assert response.status_code == 200
    assert response.content.startswith(b"PK")


def test_docx_is_served(client: TestClient, artifact_root: Path) -> None:
    """Bridge 应能服务 .docx 文件下载。"""
    docx = artifact_root / "outfit-assistant" / "outputs" / "report.docx"
    docx.parent.mkdir(parents=True)
    docx.write_bytes(b"PK\x03\x04\x00" + b"fake-docx-content")

    response = client.get(
        "/api/artifacts/outfit-assistant/files/"
        "outputs/report.docx"
    )

    assert response.status_code == 200
    assert response.content.startswith(b"PK")


def test_pdf_is_served(client: TestClient, artifact_root: Path) -> None:
    """Bridge 应能服务 .pdf 文件下载。"""
    pdf = artifact_root / "outfit-assistant" / "outputs" / "document.pdf"
    pdf.parent.mkdir(parents=True)
    pdf.write_bytes(b"%PDF-1.4" + b"fake-pdf-content")

    response = client.get(
        "/api/artifacts/outfit-assistant/files/"
        "outputs/document.pdf"
    )

    assert response.status_code == 200
    assert response.content.startswith(b"%PDF")


def test_xlsx_is_served(client: TestClient, artifact_root: Path) -> None:
    """Bridge 应能服务 .xlsx 文件下载。"""
    xlsx = artifact_root / "outfit-assistant" / "outputs" / "data.xlsx"
    xlsx.parent.mkdir(parents=True)
    xlsx.write_bytes(b"PK\x03\x04\x00" + b"fake-xlsx-content")

    response = client.get(
        "/api/artifacts/outfit-assistant/files/"
        "outputs/data.xlsx"
    )

    assert response.status_code == 200
    assert response.content.startswith(b"PK")


def test_pptx_in_package_root_is_served(client: TestClient, artifact_root: Path) -> None:
    """包根目录内有效路径的 .pptx 应被正常服务。"""
    secret = artifact_root / "outfit-assistant" / "secret.pptx"
    secret.parent.mkdir(parents=True, exist_ok=True)
    secret.write_bytes(b"valid-pptx-content")

    response = client.get(
        "/api/artifacts/outfit-assistant/files/"
        "secret.pptx"
    )

    assert response.status_code == 200
    assert b"valid-pptx-content" in response.content


# ═══════════════════════════════════════════════════════════════════
# _resolve_artifact fallback 路径覆盖（D-2 修正）
# 审查时漏读调用点误判 fallback 为死代码；实为活路径——处理无 session_id 的 legacy artifact_id。
# 此处补测试锁住 fallback 行为，防未来误删。
# ═══════════════════════════════════════════════════════════════════


def test_resolve_artifact_primary_path_flat_layout(
    artifact_root: Path,
) -> None:
    """主路径：artifact_id 无 session_id 段（3 段：agent/file_subpath…），

    直接命中 ③ workspace 扁平产物布局 `{agent}/{outputs}/{file}`。
    业务规则：产物可按 agent_id/relative_path 形式直接访问，不强求 session_id。
    """
    from routes.artifacts import _resolve_artifact

    # ③ workspace 扁平布局：outfit-assistant/outputs/look.png（3 段，无 session_id）
    flat = artifact_root / "outfit-assistant" / "outputs" / "look.png"
    flat.parent.mkdir(parents=True)
    flat.write_bytes(JPEG_BYTES)

    # artifact_id = outfit-assistant/outputs/look.png（3 段）
    # agent_pkg_name = outfit-assistant（= artifact_id.split("/")[0] = parts[0]）
    resolved = _resolve_artifact(
        "outfit-assistant/outputs/look.png",
        agent_pkg_name="outfit-assistant",
    )
    assert resolved is not None
    assert resolved.is_file()
    assert resolved.read_bytes() == JPEG_BYTES


def test_resolve_artifact_fallback_strip_session_id(
    artifact_root: Path,
) -> None:
    """Fallback 1：artifact_id 带 session_id（4 段），但 ③ 是扁平布局（无 session_id），

    strip 第二段后命中主路径。
    业务规则：daemon 产物落 ③ 是扁平的（{agent}/outputs/...），但 artifact_id 可能带
    session_id（{agent}/{session}/outputs/...）——strip 后应能命中。
    """
    from routes.artifacts import _resolve_artifact

    # ③ 扁平布局（无 session_id）
    flat = artifact_root / "outfit-assistant" / "outputs" / "look.png"
    flat.parent.mkdir(parents=True)
    flat.write_bytes(JPEG_BYTES)

    # artifact_id 带 session_id（4 段）：outfit-assistant/user_1_cli_2/outputs/look.png
    # parts[0] = outfit-assistant = agent_pkg_name → len(parts)>=3 成立 → strip parts[1]
    # → outfit-assistant/outputs/look.png（3 段）→ �命中 ③ 扁平布局
    resolved = _resolve_artifact(
        "outfit-assistant/user_1_cli_2/outputs/look.png",
        agent_pkg_name="outfit-assistant",
    )
    assert resolved is not None
    assert resolved.is_file()
    assert resolved.read_bytes() == JPEG_BYTES


def test_resolve_artifact_fallback_not_triggered_when_flat_exists(
    artifact_root: Path,
) -> None:
    """主路径优先：③ 扁平布局已存在时，不走 fallback strip。

    业务规则：strip 是兜底，不覆盖主路径命中。
    """
    from routes.artifacts import _resolve_artifact

    # ③ 同时有扁平（无 session）和带 session 的布局
    flat = artifact_root / "outfit-assistant" / "outputs" / "look.png"
    flat.parent.mkdir(parents=True)
    flat.write_bytes(JPEG_BYTES)

    with_session = artifact_root / "outfit-assistant" / "user_1_cli_2" / "outputs" / "look.png"
    with_session.parent.mkdir(parents=True)
    with_session.write_bytes(b"different-content")

    # artifact_id 带 session_id（4 段），但主路径（带 session）已命中 → 不 strip
    resolved = _resolve_artifact(
        "outfit-assistant/user_1_cli_2/outputs/look.png",
        agent_pkg_name="outfit-assistant",
    )
    assert resolved is not None
    assert resolved.is_file()
    # 主路径命中带 session 的文件，不是 strip 后的扁平文件
    assert resolved.read_bytes() == b"different-content"
    assert resolved != flat.resolve()


def test_resolve_artifact_returns_none_when_not_found(
    artifact_root: Path,
) -> None:
    """文件不存在时返回 None（或非文件 Path），不抛异常。

    业务规则：404 由调用方决定，_resolve_artifact 只做解析。
    """
    from routes.artifacts import _resolve_artifact

    resolved = _resolve_artifact(
        "outfit-assistant/outputs/nonexistent.png",
        agent_pkg_name="outfit-assistant",
    )
    # 主路径不命中，fallback strip 后也不命中 → 返回 None 或非文件 Path
    assert resolved is None or not resolved.is_file()


def test_resolve_artifact_traversal_blocked_returns_none(
    artifact_root: Path,
) -> None:
    """路径越界 ③ 根时，主路径返回 None 并记日志，不抛异常。

    业务规则：traversal 防护在 _resolve_artifact 内，越界不命中文件。
    """
    from routes.artifacts import _resolve_artifact

    # 越界路径：artifact_id 含 .. 逃逸 ③ 根
    # 注：调用方（get_artifact_file）会先做 traversal 校验抛 403，
    # _resolve_artifact 内部用 relative_to 兜底，越界时 candidate=None
    resolved = _resolve_artifact(
        "../../../../etc/passwd",
        agent_pkg_name="",
    )
    # parts[0] = ".." != agent_pkg_name("") → fallback 不触发；
    # 主路径 relative_to 校验失败 → candidate=None
    assert resolved is None or not resolved.is_file()
