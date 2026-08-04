"""Contract tests for artifact URLs in REST and WebSocket chat content."""

from __future__ import annotations

import pytest

from config import settings
from models import ArtifactInfo
from routes import chat, chat_rest
from routes.artifact_extractor import extract_artifact_infos


RELATIVE = (
    "结果：![look](/api/artifacts/outfit-assistant/files/"
    "outfit-assistant/outputs/task-1/look.png)"
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


def test_custom_label_artifact_is_extracted() -> None:
    """LLM 用自定义 label 贴的 artifact markdown 也应被提取。

    边界是 url 指向 ``/api/artifacts/`` 路径，不是 label 文本（``生成图片 N``）。
    LLM 自由 label 如 ``![居家穿搭效果图](url)`` + artifact API url = 产物；
    用户随手 ``![表情](https://emoji.example.com/x.png)`` = 非产物（跳过）。
    """
    content = (
        "方案已生成\n\n"
        "![居家穿搭效果图](http://localhost:8199/health/api/artifacts/"
        "outfit-assistant/files/outputs/look.png)"
    )
    artifacts = extract_artifact_infos(content)
    assert len(artifacts) == 1, f"自定义 label artifact 应提取 1 个, got {len(artifacts)}"
    assert artifacts[0].type == "image"
    assert artifacts[0].name == "look.png"


def test_non_artifact_url_is_not_extracted() -> None:
    """指向非 artifact API 的 markdown 图片链接不应被提取为产物。

    守 I-1：不误匹配用户正文里的随手图片引用（emoji、外部图床等），
    只有 url 指向 ``/api/artifacts/`` 路径才是 agent 产物。
    """
    content = (
        "看看这个表情 ![哈哈](https://emoji.example.com/smile.png)\n\n"
        "外部图床 ![photo](https://images.example.com/photo.jpg)"
    )
    artifacts = extract_artifact_infos(content)
    assert artifacts == [], f"非 artifact API url 不应被提取, got {artifacts}"


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


# ═══════════════════════════════════════════════════════════
# 产物输出模式（apply_artifact_output_mode）— 两维正交开关
#   artifact_reference: link | placeholder
#   artifact_payload:   url | base64
# ═══════════════════════════════════════════════════════════

_ART = ArtifactInfo(
    type="image",
    url="/api/artifacts/outfit-assistant/files/outputs/look.png",
    name="look.png",
    mime_type="image/png",
)
_CONTENT = "结果：![生成图片 1](/api/artifacts/outfit-assistant/files/outputs/look.png)"


def test_output_mode_link_url_is_noop(monkeypatch) -> None:
    """link + url（默认）：文本与元数据原样，无转换。"""
    from routes.artifact_extractor import apply_artifact_output_mode

    monkeypatch.setattr(settings, "artifact_reference", "link")
    monkeypatch.setattr(settings, "artifact_payload", "url")
    content, artifacts = apply_artifact_output_mode(_CONTENT, [_ART.model_copy(deep=True)])
    assert content == _CONTENT
    assert artifacts[0].url == _ART.url
    assert artifacts[0].data_base64 == ""
    assert artifacts[0].placeholder == ""


def test_output_mode_link_base64_inlines_data_uri(monkeypatch) -> None:
    """link + base64：文本中产物引用替换为 data URI，元数据填 data_base64。"""
    from routes.artifact_extractor import apply_artifact_output_mode

    monkeypatch.setattr(settings, "artifact_reference", "link")
    monkeypatch.setattr(settings, "artifact_payload", "base64")
    monkeypatch.setattr(
        "routes.artifact_extractor.read_artifact_base64",
        lambda url: "aGVsbG8=",  # "hello"
    )
    content, artifacts = apply_artifact_output_mode(_CONTENT, [_ART.model_copy(deep=True)])
    assert content == "结果：![生成图片 1](data:image/png;base64,aGVsbG8=)"
    assert artifacts[0].data_base64 == "aGVsbG8="
    assert artifacts[0].placeholder == ""


def test_output_mode_placeholder_url_keeps_url(monkeypatch) -> None:
    """placeholder + url：文本替换为 {{artifact:N}}，元数据保留 url 并附 placeholder。"""
    from routes.artifact_extractor import apply_artifact_output_mode

    monkeypatch.setattr(settings, "artifact_reference", "placeholder")
    monkeypatch.setattr(settings, "artifact_payload", "url")
    content, artifacts = apply_artifact_output_mode(_CONTENT, [_ART.model_copy(deep=True)])
    assert content == "结果：![生成图片 1]({{artifact:1}})"
    assert artifacts[0].placeholder == "{{artifact:1}}"
    assert artifacts[0].url == _ART.url
    assert artifacts[0].data_base64 == ""


def test_output_mode_placeholder_base64_embeds_content(monkeypatch) -> None:
    """placeholder + base64：文本占位符 + 元数据附 placeholder 与 data_base64。"""
    from routes.artifact_extractor import apply_artifact_output_mode

    monkeypatch.setattr(settings, "artifact_reference", "placeholder")
    monkeypatch.setattr(settings, "artifact_payload", "base64")
    monkeypatch.setattr(
        "routes.artifact_extractor.read_artifact_base64",
        lambda url: "aGVsbG8=",
    )
    content, artifacts = apply_artifact_output_mode(_CONTENT, [_ART.model_copy(deep=True)])
    assert content == "结果：![生成图片 1]({{artifact:1}})"
    assert artifacts[0].placeholder == "{{artifact:1}}"
    assert artifacts[0].data_base64 == "aGVsbG8="
    assert artifacts[0].url == _ART.url


def test_output_mode_multiple_artifacts_numbered(monkeypatch) -> None:
    """多个产物时占位符按出现顺序编号 {{artifact:1}} / {{artifact:2}}。"""
    from routes.artifact_extractor import apply_artifact_output_mode

    monkeypatch.setattr(settings, "artifact_reference", "placeholder")
    monkeypatch.setattr(settings, "artifact_payload", "base64")
    monkeypatch.setattr("routes.artifact_extractor.read_artifact_base64", lambda url: "b64")
    art2 = ArtifactInfo(
        type="download",
        url="/api/artifacts/outfit-assistant/files/outputs/deck.pptx",
        name="deck.pptx",
        mime_type="application/vnd.openxmlformats-officedocument.presentationml.presentation",
    )
    content = (
        "图：![a](/api/artifacts/outfit-assistant/files/outputs/look.png)\n"
        "文件：[下载 1](/api/artifacts/outfit-assistant/files/outputs/deck.pptx)"
    )
    new_content, artifacts = apply_artifact_output_mode(content, [_ART, art2])
    assert "{{artifact:1}}" in new_content and "{{artifact:2}}" in new_content
    assert artifacts[0].placeholder == "{{artifact:1}}"
    assert artifacts[1].placeholder == "{{artifact:2}}"
