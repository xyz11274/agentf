"""Tests for content_serializer — multimodal content processing.

Covers:
- Plain string passthrough → content array with single text part
- Content array with text only
- Content array with image_url (data: URL) → flattened to text part
- Content array with mix of text and images
- Empty content
"""
from __future__ import annotations

import json

import pytest

from content_serializer import (
    flatten_to_text,
    normalize_content,
    prepare_content_for_daemon,
)
from models import ContentPart, ImagePart, TextPart


def _parse_af_input_inline(raw: str) -> list[dict]:
    """Inline version of parse_af_input — avoids cross-package import path issues."""
    if not raw:
        return []
    try:
        parsed = json.loads(raw)
        if isinstance(parsed, list) and all(isinstance(p, dict) for p in parsed):
            return parsed
    except json.JSONDecodeError:
        pass
    return [{"type": "text", "text": raw}]


# ── normalize_content ─────────────────────────────────────────────


class TestNormalizeContent:
    def test_plain_string_becomes_single_text_part(self) -> None:
        result = normalize_content("你好")
        assert result == [{"type": "text", "text": "你好"}]

    def test_empty_string_returns_empty_list(self) -> None:
        assert normalize_content("") == []

    def test_pydantic_text_part(self) -> None:
        result = normalize_content([TextPart(type="text", text="hello")])
        assert result == [{"type": "text", "text": "hello"}]

    def test_pydantic_image_part(self) -> None:
        result = normalize_content([
            ImagePart(type="image_url", image_url={"url": "data:image/png;base64,aGVsbG8="}),
        ])
        assert result == [{"type": "image_url", "image_url": {"url": "data:image/png;base64,aGVsbG8="}}]

    def test_raw_dict_list(self) -> None:
        result = normalize_content([{"type": "text", "text": "raw"}])
        assert result == [{"type": "text", "text": "raw"}]

    def test_skips_malformed_parts(self) -> None:
        result = normalize_content([{"type": "text", "text": "ok"}, {"unexpected": True}])
        assert result == [{"type": "text", "text": "ok"}]


# ── flatten_to_text ────────────────────────────────────────────────


class TestFlattenToText:
    def test_text_parts_pass_through(self, tmp_path) -> None:
        parts = [{"type": "text", "text": "hello"}, {"type": "text", "text": "world"}]
        result = flatten_to_text(parts, upload_dir=tmp_path)
        assert result == parts

    def test_image_url_with_data_is_flattened(self, tmp_path) -> None:
        # Simulate the real upload_dir layout: {agent_packages}/{agent_id}/workspace/{session}/upload
        # (see content_serializer._compute_upload_dir). flatten_to_text drops the
        # {agent_packages}/{agent_id}/ prefix to produce a sandbox-relative path
        # workspace/{session}/upload/{sha}.{ext} — see agent-package-structure.md §3.6 I-7.
        upload_dir = tmp_path / "test-agent" / "workspace" / "user1_client-a" / "upload"
        agent_root = tmp_path / "test-agent"
        parts = [{"type": "image_url", "image_url": {"url": "data:image/png;base64,aGVsbG8="}}]
        result = flatten_to_text(parts, upload_dir=upload_dir, agent_root=agent_root)
        assert len(result) == 1
        assert result[0]["type"] == "text"
        assert "用户上传了图片" in result[0]["text"]
        # sandbox-relative path: drop {agent_packages}/{agent_id}/ prefix
        assert "workspace/user1_client-a/upload/" in result[0]["text"]
        assert "test-agent" not in result[0]["text"], f"got: {result[0]['text']!r}"
        assert str(tmp_path) not in result[0]["text"], f"got: {result[0]['text']!r}"

    def test_mixed_parts(self, tmp_path) -> None:
        parts = [
            {"type": "text", "text": "看图"},
            {"type": "image_url", "image_url": {"url": "data:image/png;base64,aGVsbG8="}},
            {"type": "text", "text": "好看吗"},
        ]
        result = flatten_to_text(parts, upload_dir=tmp_path)
        assert len(result) == 3
        assert result[0]["type"] == "text"
        assert result[0]["text"] == "看图"
        assert result[1]["type"] == "text"
        assert "用户上传了图片" in result[1]["text"]
        assert result[2]["type"] == "text"
        assert result[2]["text"] == "好看吗"


# ── extract_user_text (coordinator side) ──────────────────────────


class TestExtractUserText:
    """Tests the coordinator helper that extracts user text from AF_INPUT."""

    @staticmethod
    def _extract(content_raw: str) -> str:
        """Replicates the coordinator's extract_user_text logic."""
        parts = _parse_af_input_inline(content_raw)
        texts = [p["text"] for p in parts if p.get("type") == "text"]
        return "\n".join(texts) if texts else content_raw

    def test_old_format_plain_string(self) -> None:
        assert self._extract("帮我推荐穿搭") == "帮我推荐穿搭"

    def test_new_format_single_text(self) -> None:
        raw = json.dumps([{"type": "text", "text": "帮我推荐登山穿搭"}])
        assert self._extract(raw) == "帮我推荐登山穿搭"

    def test_new_format_multi_text(self) -> None:
        raw = json.dumps([
            {"type": "text", "text": "看图"},
            {"type": "text", "text": "这张衣服怎么样"},
        ])
        assert self._extract(raw) == "看图\n这张衣服怎么样"

    def test_content_array_with_image_path(self) -> None:
        raw = json.dumps([
            {"type": "text", "text": "看图"},
            {"type": "text", "text": "用户上传了图片，路径: /tmp/abc.png"},
        ])
        text = self._extract(raw)
        assert "看图" in text
        assert "用户上传了图片" in text
        assert "read_image" not in text

    def test_empty_input_returns_empty(self) -> None:
        assert self._extract("") == ""


# ── prepare_content_for_daemon (full pipeline) ────────────────────


class TestPrepareContentForDaemon:
    """Regression: prepare_content_for_daemon MUST return plain text,
    NOT JSON. Daemon sets this as AF_INPUT for the subprocess env,
    and JSON strings get truncated by the shell command construction."""

    def test_plain_string_passthrough(self) -> None:
        result = prepare_content_for_daemon("帮我推荐穿搭")
        assert result == "帮我推荐穿搭"
        assert not result.startswith("["), f"should not be JSON: {result!r}"

    def test_text_content_array(self) -> None:
        result = prepare_content_for_daemon([TextPart(type="text", text="hello")])
        assert result == "hello"
        assert not result.startswith("["), f"should not be JSON: {result!r}"

    def test_content_array_with_image_is_flattened_when_agent_id_given(self) -> None:
        result = prepare_content_for_daemon([
            TextPart(type="text", text="看图"),
            ImagePart(type="image_url", image_url={"url": "data:image/png;base64,aGVsbG8="}),
        ], agent_id="test-agent", user_id="user1", client_id="cli-a")
        assert "看图" in result
        assert "用户上传了图片" in result
        # sandbox-relative path (drop the agent_id/ prefix), see agent-package-structure.md §3.6 I-7
        assert "workspace/user1_cli-a/upload/" in result
        assert "test-agent" not in result, f"path must be sandbox-relative, got: {result!r}"
        assert "read_image" not in result
        assert not result.startswith("["), f"should not be JSON: {result!r}"

    def test_chinese_characters_preserved(self) -> None:
        """Regression: Chinese chars must not be escaped or truncated."""
        result = prepare_content_for_daemon("帮我推荐登山穿搭，其他的不要问")
        assert result == "帮我推荐登山穿搭，其他的不要问"

    def test_special_chars_that_broke_shell_env(self) -> None:
        """Regression: chars like { } [ ] \" ' would break daemon's
        shell command construction. Ensure they pass through as-is."""
        content = '帮我推荐{穿搭} [外套] 他说"好看"'
        result = prepare_content_for_daemon(content)
        assert result == content

    def test_mixed_chinese_ascii(self) -> None:
        """Regression: mix of Chinese and ASCII that previously caused
        daemon JSON-RPC double-parse issues."""
        result = prepare_content_for_daemon("穿这件L码的外套怎么样? model=test")
        assert result == "穿这件L码的外套怎么样? model=test"

    def test_empty_string(self) -> None:
        result = prepare_content_for_daemon("")
        assert result == ""

    def test_content_array_empty_text(self) -> None:
        result = prepare_content_for_daemon([TextPart(type="text", text="")])
        assert result == ""

    def test_multiline_text(self) -> None:
        content = "第一行\n第二行\n第三行"
        result = prepare_content_for_daemon(content)
        assert result == content

    def test_no_agent_id_means_images_are_dropped(self) -> None:
        """Without agent_id, the function skips image processing entirely
        and only returns text part content."""
        result = prepare_content_for_daemon([
            TextPart(type="text", text="test"),
            ImagePart(type="image_url", image_url={"url": "data:image/png;base64,aGVsbG8="}),
        ])
        assert result == "test"  # image part silently skipped
