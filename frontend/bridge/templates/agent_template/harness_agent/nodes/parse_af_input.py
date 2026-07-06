"""parse_af_input — node 端统一 content 解析入口。

Phase A 多模态升级：AF_INPUT env 可能是 JSON 对象字符串（{"stream", "messages"}）
也可能是旧版纯文本字符串。此函数统一解析，确保向后兼容。

用法（在任一 node 脚本中）：
    from parse_af_input import parse_af_input, parse_stream
    parts = parse_af_input(os.environ.get("AF_INPUT", ""))
    stream = parse_stream(os.environ.get("AF_INPUT", ""))

返回值：
    list[dict] — content array，[{type, text}, {type, image_url, image_url: {url}}]
    空输入返回空列表 []。

Backward compatible:
    - 旧版纯字符串 → [{type: text, text: raw_str}]
    - JSON array 原样返回
    - JSON 对象（含 messages 字段）→ 解包 message.content 为 content array
"""

from __future__ import annotations

import json
import logging
from typing import Any

logger = logging.getLogger("agentflow.node.parse_af_input")


def _try_parse(raw: str) -> Any | None:
    """尝试 JSON 解析，失败返回 None。"""
    if not raw:
        return None
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        return None


def parse_stream(raw: str) -> bool:
    """从 AF_INPUT 中提取 stream 标记。

    新格式: {"stream": true, "messages": [...]}
    旧格式（纯文本/数组） → 返回 False
    """
    parsed = _try_parse(raw)
    if isinstance(parsed, dict) and "stream" in parsed:
        return bool(parsed["stream"])
    return False


def parse_af_input(raw: str) -> list[dict[str, Any]]:
    """Parse AF_INPUT env value into a list of content part dicts.

    Backward-compatible: for a plain string returns
    ``[{type: text, text: raw}]``.

    I-6 invariant: old string content and new array content converge to
    the same ``list[dict]`` type downstream.
    """
    if not raw:
        return []

    # 新协议: {"stream": bool, "messages": [{role, content}, ...]}
    # 提取每一条 message 的 content 组装为 content array
    parsed = _try_parse(raw)
    if isinstance(parsed, dict) and "messages" in parsed:
        parts: list[dict[str, Any]] = []
        for msg in parsed["messages"]:
            content = msg.get("content", "")
            if isinstance(content, str) and content:
                parts.append({"type": "text", "text": content})
            elif isinstance(content, list):
                parts.extend(content)
        return parts

    if isinstance(parsed, list):
        # Already a content array — validate structure minimally
        if all(isinstance(p, dict) for p in parsed):
            return parsed
        # Some items are not dicts — treat as atomic string
        logger.warning(
            "parse_af_input: JSON array contains non-dict items, "
            "falling back to string wrapper"
        )
        return [{"type": "text", "text": raw}]

    # JSON non-array — wrap as text
    if parsed is not None:
        return [{"type": "text", "text": raw}]

    # Plain string — backward compatible
    return [{"type": "text", "text": raw}]
