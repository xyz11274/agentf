"""Contract tests for artifact URLs in REST and WebSocket chat content."""

from __future__ import annotations

import pytest

from models import ArtifactInfo
from routes import chat, chat_rest
from routes.artifact_extractor import extract_artifact_infos


RELATIVE = (
    "结果：![look](/api/artifacts/outfit-assistant/files/"
    "outfit_assistant/outputs/task-1/look.png)"
)


@pytest.mark.parametrize(
    "publicizer",
    [chat.publicize_artifact_urls, chat_rest.publicize_artifact_urls],
    ids=["websocket", "rest"],
)
def test_public_and_relative_artifact_url_modes(
    monkeypatch: pytest.MonkeyPatch,
    publicizer,
) -> None:
    """TC-brg-013: REST 与 WS 使用一致的 Artifact 公网 URL 规则。

    业务规则:
    - 未配置公网地址时保留相对 URL；配置后返回外部可访问的绝对 URL。
    - REST 与 WS 不能出现不同配置语义。

    错误模型:
    - 只修改一个聊天入口；尾部斜杠造成 ``//api``；重复改写绝对 URL。

    失败意味着:
    - 内置 UI 或外部服务至少有一个入口无法访问生成产物。
    """
    monkeypatch.setattr(chat.settings, "artifact_public_base_url", "")
    assert publicizer(RELATIVE) == RELATIVE

    monkeypatch.setattr(
        chat.settings,
        "artifact_public_base_url",
        "https://agent.example.com/",
    )
    absolute = publicizer(RELATIVE)
    assert "https://agent.example.com/api/artifacts/" in absolute
    assert "example.com//api" not in absolute
    assert "/Users/" not in absolute

    assert publicizer(absolute) == absolute


# ═══════════════════════════════════════════════════════════════════
# Structured artifact extraction (Plan B)
# ═══════════════════════════════════════════════════════════════════

CONTENT_WITH_ARTIFACTS = (
    "PPT 生成完成\n\n"
    "## 生成产物\n\n"
    "![生成图片 1](/api/artifacts/outfit-assistant/files/agent/outputs/look.png)\n\n"
    "[查看报告 1](/api/artifacts/outfit-assistant/files/agent/outputs/report.html)\n\n"
    "[📥 下载文件 1](/api/artifacts/outfit-assistant/files/agent/outputs/deck.pptx)"
)


def test_extract_images_from_content() -> None:
    """Image artifact patterns produce ArtifactInfo with type=image."""
    artifacts = extract_artifact_infos(CONTENT_WITH_ARTIFACTS)
    images = [a for a in artifacts if a.type == "image"]
    assert len(images) == 1
    assert images[0].name == "look.png"
    assert images[0].mime_type == "image/png"


def test_extract_html_from_content() -> None:
    """HTML report patterns produce ArtifactInfo with type=html."""
    artifacts = extract_artifact_infos(CONTENT_WITH_ARTIFACTS)
    htmls = [a for a in artifacts if a.type == "html"]
    assert len(htmls) == 1
    assert htmls[0].name == "report.html"
    assert htmls[0].mime_type == "text/html"


def test_extract_downloads_from_content() -> None:
    """Download patterns produce ArtifactInfo with type=download."""
    artifacts = extract_artifact_infos(CONTENT_WITH_ARTIFACTS)
    downloads = [a for a in artifacts if a.type == "download"]
    assert len(downloads) == 1
    assert downloads[0].name == "deck.pptx"
    assert downloads[0].mime_type == "application/vnd.openxmlformats-officedocument.presentationml.presentation"


def test_extract_all_artifact_types() -> None:
    """All three artifact types are extracted from mixed content."""
    artifacts = extract_artifact_infos(CONTENT_WITH_ARTIFACTS)
    assert len(artifacts) == 3
    types = {a.type for a in artifacts}
    assert types == {"image", "html", "download"}


def test_empty_content_returns_empty_list() -> None:
    """Content without artifact patterns returns empty list."""
    assert extract_artifact_infos("你好，今天穿什么？") == []


def test_progress_chunk_has_no_artifacts() -> None:
    """Progress chunks (non-final) should not have artifacts."""
    progress = "正在执行 outfit-stylist…"
    assert extract_artifact_infos(progress) == []


def test_artifact_info_has_correct_url() -> None:
    """Artifact URL is preserved as-is from the content."""
    artifacts = extract_artifact_infos(CONTENT_WITH_ARTIFACTS)
    assert len(artifacts) >= 1
    assert all(a.url.startswith("/api/artifacts/") for a in artifacts)
    assert all(a.url for a in artifacts)  # non-empty


def test_download_artifact_preserves_filename() -> None:
    """Download filename is extracted from the URL path."""
    content = "[📥 下载文件 1](/api/artifacts/test/files/my_report.pptx)"
    artifacts = extract_artifact_infos(content)
    assert len(artifacts) == 1
    assert artifacts[0].name == "my_report.pptx"
    assert artifacts[0].type == "download"


def test_pdf_download_has_correct_mime() -> None:
    """.pdf download has pdf MIME type."""
    content = "[📥 下载文件 1](/api/artifacts/test/files/doc.pdf)"
    artifacts = extract_artifact_infos(content)
    assert len(artifacts) == 1
    assert artifacts[0].mime_type == "application/pdf"


def test_publicized_urls_are_deduplicated() -> None:
    """Same artifact URL appearing twice is only reported once."""
    content = (
        "[📥 下载文件 1](/api/artifacts/test/files/report.pptx)\n\n"
        "[📥 下载文件 2](/api/artifacts/test/files/report.pptx)"
    )
    artifacts = extract_artifact_infos(content)
    assert len(artifacts) == 1


def test_chatchunk_serializes_artifacts() -> None:
    """ChatChunk model dump includes artifacts field."""
    from models import ChatChunk

    chunk = ChatChunk(
        content="完成",
        final=True,
        artifacts=[
            ArtifactInfo(type="download", url="/api/artifacts/test/f.pptx", name="f.pptx"),
        ],
    )
    dumped = chunk.model_dump()
    assert "artifacts" in dumped
    assert len(dumped["artifacts"]) == 1
    assert dumped["artifacts"][0]["type"] == "download"
    assert dumped["artifacts"][0]["url"] == "/api/artifacts/test/f.pptx"
