"""Contract guard: sidecar → bridge → daemon message interface.

守护 §10 修复的接口契约（docs/bugs/20260724_sidecar_build.md）：
1. sidecar 传给 bridge 的 content 必须是 **ContentPart 数组**（list[ContentPart]），
   不能是字符串、不能是嵌套 messages 结构（含 role）。
2. bridge 收到数组后 normalize → flatten_to_text（图片落盘）→ 纯文本。
3. daemon 期望 messages[0].content 为纯文本（无 JSON 数组、无嵌套）。

本测试模拟 sidecar 的 content 构造逻辑（与 sidecar_router.cpp / main.cpp
中 rest_body["content"] = content_parts 对齐），验证其输出能通过 bridge 的
prepare_content_for_daemon 产生 daemon 期望的纯文本。
"""
from __future__ import annotations

import json

import pytest

from content_serializer import (
    flatten_to_text,
    normalize_content,
    prepare_content_for_daemon,
)


# ── sidecar 侧 content 构造（对齐 sidecar_router.cpp / main.cpp）──────


def sidecar_build_content_parts(user_input_text: str,
                                user_input_object_str: str = "") -> list[dict]:
    """模拟 sidecar 从上游 AccessCtrl 构造 ContentPart 数组。

    与 sidecar_router.cpp HandleUserRequest 的构造逻辑一一对应：
    - UserInputText → {type:text, text:...}
    - UserInputObject.data.content[] 中 type==image → {type:image_url, image_url:{url:data}}
    """
    parts: list[dict] = []
    if user_input_text:
        parts.append({"type": "text", "text": user_input_text})
    if user_input_object_str:
        try:
            uio = json.loads(user_input_object_str)
        except json.JSONDecodeError:
            return parts
        for item in (uio.get("data") or {}).get("content") or []:
            if item.get("type") == "image":
                parts.append({
                    "type": "image_url",
                    "image_url": {"url": item.get("data", "")},
                })
    return parts


def sidecar_rest_body(parts: list[dict], user_id: str, client_id: str) -> dict:
    """模拟 sidecar rest_body（修复后：content 直接传数组，不字符串化、不包 messages）。"""
    return {"user_id": user_id, "client_id": client_id, "content": parts}


# ── 契约 1：content 必须是 ContentPart 数组 ──────────────────────────


class TestSidecarContentIsArray:
    def test_pure_text_is_array_not_string(self) -> None:
        parts = sidecar_build_content_parts("帮我拍一张照片看一下今天的穿搭")
        body = sidecar_rest_body(parts, "user1", "cli1")
        content = body["content"]
        assert isinstance(content, list), \
            f"content must be a list, got {type(content).__name__}: {content!r}"
        assert content == [{"type": "text", "text": "帮我拍一张照片看一下今天的穿搭"}]

    def test_multimodal_is_array_with_image_url(self) -> None:
        uio = json.dumps({"code": 200101, "data": {
            "content": [{"type": "image", "data": "data:image/png;base64,aGVsbG8="}]}})
        parts = sidecar_build_content_parts("帮我拍一张照片", uio)
        assert parts == [
            {"type": "text", "text": "帮我拍一张照片"},
            {"type": "image_url", "image_url": {"url": "data:image/png;base64,aGVsbG8="}},
        ]

    def test_no_role_or_messages_nesting(self) -> None:
        """守护双层嵌套回归：content 数组元素不得含 role/messages。"""
        uio = json.dumps({"code": 200101, "data": {
            "content": [{"type": "image", "data": "data:image/png;base64,aGVsbG8="}]}})
        parts = sidecar_build_content_parts("帮我拍一张照片", uio)
        for p in parts:
            assert "role" not in p and "messages" not in p, \
                f"content part must not nest message structure: {p}"


# ── 契约 2：bridge 处理后输出纯文本（daemon 期望格式）────────────────


class TestBridgeProducesPlainTextForDaemon:
    """daemon 期望 messages[0].content 为纯文本；JSON 数组/嵌套会导致
    daemon shell 命令构造截断（prepare_content_for_daemon 注释明示）。"""

    def test_pure_text_end_to_end(self) -> None:
        parts = sidecar_build_content_parts("帮我拍一张照片看一下今天的穿搭")
        result = prepare_content_for_daemon(parts, agent_id="outfit-assistant",
                                            user_id="user1", client_id="cli1")
        assert result == "帮我拍一张照片看一下今天的穿搭"
        assert not result.startswith("["), f"should not be JSON: {result!r}"
        assert "role" not in result and "messages" not in result

    def test_multimodal_end_to_end_flattens_image(self, tmp_path) -> None:
        """bridge 收到数组 → normalize 拆出 image_url → flatten 落盘 → 纯文本。"""
        uio = json.dumps({"code": 200101, "data": {
            "content": [{"type": "image", "data": "data:image/png;base64,aGVsbG8="}]}})
        parts = sidecar_build_content_parts("帮我拍一张照片", uio)
        # 用 tmp_path 隔离落盘目录（模拟 settings.agent_packages_path）
        result = prepare_content_for_daemon(
            parts,
            agent_id="outfit-assistant",
            user_id="user1", client_id="cli1",
        )
        assert "帮我拍一张照片" in result
        assert "用户上传了图片" in result
        assert "workspace/user1_cli1/upload/" in result
        assert not result.startswith("["), f"should not be JSON: {result!r}"

    def test_stringified_array_is_rejected_by_contract(self) -> None:
        """契约守护：若 sidecar 误传 json.dumps 字符串，bridge 会把它当纯文本
        （图片不落盘）——此路径必须被测试拦截，防止回归。"""
        uio = json.dumps({"code": 200101, "data": {
            "content": [{"type": "image", "data": "data:image/png;base64,aGVsbG8="}]}})
        parts = sidecar_build_content_parts("帮我拍一张照片", uio)
        wrong_body = {"user_id": "u", "client_id": "c",
                      "content": json.dumps(parts, ensure_ascii=False)}  # 错误传法
        assert isinstance(wrong_body["content"], str), "precondition"
        result = prepare_content_for_daemon(wrong_body["content"],
                                            agent_id="outfit-assistant",
                                            user_id="u", client_id="c")
        # 字符串被当纯文本 → 输出仍含 JSON 痕迹（不符合 daemon 期望）
        assert result.startswith("[") or "image_url" in result, \
            "stringified array must NOT silently produce valid daemon content"
