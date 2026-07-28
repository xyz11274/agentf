"""测试 sidecar 对 nested OpenAI JSON 的纯文本提取逻辑。

sidecar 从 Bridge SSE 的 choices[0].message.content 提取 token，
如果 token 本身是 raw OpenAI JSON（coordinator 写入 databus 时未剥壳），
sidecar 会递归提取 choices[0].message.content 或 choices[0].delta.content 的纯文本。
"""

import json
from typing import Any

SAMPLE_NESTED_JSON = '''{"id":"test-001","choices":[{"index":0,"delta":{"content":"穿搭方案如下\\n1. 上装：白色衬衫\\n2. 下装：黑色长裤","role":"assistant","tool_calls":[]}}],"created":1234567890,"model":"MiniMax-M3"}'''

SAMPLE_CLEAN_TEXT = "穿搭方案如下\n1. 上装：白色衬衫\n2. 下装：黑色长裤"


def extract_clean_text(token: str) -> str:
    """模拟 sidecar 的 nested JSON 提取逻辑（main.cpp L307-328 的等效逻辑）。"""
    if len(token) > 2 and token[0] == '{':
        try:
            parsed = json.loads(token)
            # 尝试 choices[0].message.content
            msg = parsed.get("choices", [{}])[0].get("message")
            if msg and msg.get("content"):
                return msg["content"]
            # 尝试 choices[0].delta.content
            delta = parsed.get("choices", [{}])[0].get("delta")
            if delta and delta.get("content"):
                return delta["content"]
        except (json.JSONDecodeError, IndexError, KeyError):
            pass
    return token


def test_nested_json_message_content() -> None:
    """choices[0].message.content 嵌套 raw OpenAI JSON 的场景。"""
    # 模拟 Bridge 包装后的 content
    bridge_wrapped = json.dumps({
        "choices": [{"message": {"content": SAMPLE_NESTED_JSON}}]
    })
    # sidecar 提取 choices[0].message.content
    raw = json.loads(bridge_wrapped)["choices"][0]["message"]["content"]
    # 递归提取纯文本
    clean = extract_clean_text(raw)
    assert clean == SAMPLE_CLEAN_TEXT, f"expected clean text, got: {clean[:100]}"
    print(f"  ✅ message.content nested JSON → 纯文本: {clean[:60]}...")


def test_nested_json_delta_content() -> None:
    """choices[0].delta.content 嵌套 raw OpenAI JSON 的场景。"""
    # 模拟一个仅有 delta.content 的 JSON
    nested = json.dumps({
        "choices": [{"delta": {"content": "逐 token 流式输出"}}]
    })
    clean = extract_clean_text(nested)
    assert clean == "逐 token 流式输出", f"expected delta content, got: {clean}"
    print(f"  ✅ delta.content nested JSON → 纯文本: {clean}")


def test_clean_text_passthrough() -> None:
    """已经是纯文本的 content 不被修改。"""
    texts = ["你好", "穿搭方案如下", "outfit-stylist → 正在调用 bash"]
    for t in texts:
        assert extract_clean_text(t) == t, f"clean text should passthrough: {t}"
    print(f"  ✅ 纯文本原样透传")


def test_non_json_plain_text() -> None:
    """非 JSON 的纯文本（如 emoji、Markdown）不受影响。"""
    texts = [
        " 🧥 **单品搭配**：",
        " ✅ 历史记录已写回",
        " ![图片](http://localhost:8199/health/api/artifacts/outfit-assistant/files/outfit.png)",
    ]
    for t in texts:
        assert extract_clean_text(t) == t
    print(f"  ✅ 非 JSON 纯文本原样透传")


def test_empty_token() -> None:
    """空 token 或单字符 token 不触发 JSON 解析。"""
    assert extract_clean_text("") == ""
    assert extract_clean_text("a") == "a"
    print(f"  ✅ 空/单字符 token 不触发 JSON 解析")


def test_sse_wrapper_format() -> None:
    """模拟 Bridge SSE 输出完整格式，sidecar 提取 + 递归剥壳。"""
    sse_payload = json.dumps({
        "choices": [{"message": {"content": SAMPLE_NESTED_JSON}}],
        "_chunk_type": "progress",
        "_progress": True,
    })
    # sidecar 提取 choices[0].message.content
    payload = json.loads(sse_payload)
    token = payload["choices"][0]["message"]["content"]
    # 递归提取
    clean = extract_clean_text(token)
    assert clean == SAMPLE_CLEAN_TEXT, f"SSE wrapper: expected clean text"
    print(f"  ✅ SSE wrapper 格式 → 纯文本: {clean[:60]}...")


if __name__ == "__main__":
    print("=== sidecar nested JSON 提取逻辑测试 ===\n")
    test_nested_json_message_content()
    test_nested_json_delta_content()
    test_clean_text_passthrough()
    test_non_json_plain_text()
    test_empty_token()
    test_sse_wrapper_format()
    print("\n全部通过 ✅")
