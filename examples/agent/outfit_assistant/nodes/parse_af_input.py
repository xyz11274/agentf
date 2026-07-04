"""parse_af_input — node 端统一 content 解析入口。

Phase A 多模态升级：AF_INPUT env 可能是 JSON 字符串（content array）
也可能是旧版纯文本字符串。此函数统一解析，确保向后兼容。

用法（在任一 node 脚本中）：
    from parse_af_input import parse_af_input
    parts = parse_af_input(os.environ.get("AF_INPUT", ""))

返回值：
    list[dict] — content array，[{type, text}, {type, image_url, image_url: {url}}]
    空输入返回空列表 []。

Backward compatible:
    - 旧版纯字符串 → [{type: text, text: raw_str}]
    - JSON array 原样返回
    - JSON 非 array 对象或非法 JSON → 兜底字符串包装
"""

from __future__ import annotations

import json
import logging
from typing import Any

logger = logging.getLogger("agentflow.node.parse_af_input")


def parse_af_input(raw: str) -> list[dict[str, Any]]:
    """Parse AF_INPUT env value into a list of content part dicts.

    Backward-compatible: for a plain string returns
    ``[{type: text, text: raw}]``.

    I-6 invariant: old string content and new array content converge to
    the same ``list[dict]`` type downstream.
    """
    if not raw:
        return []

    try:
        parsed = json.loads(raw)
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
        return [{"type": "text", "text": raw}]
    except json.JSONDecodeError:
        # Plain string — backward compatible
        return [{"type": "text", "text": raw}]
