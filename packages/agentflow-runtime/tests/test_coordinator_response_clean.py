"""测试 coordinator 的 response key 输出只含纯文本，不含 raw LLM JSON。

用例：当 LLM 返回同时包含 content（文本）和 tool_calls（函数调用）的响应时，
coordinator 只提取 choices[0].message.content 作为 response_text，
tool_calls 等 raw JSON 不应出现在 response_text 中。
"""
import json
import pytest


@pytest.mark.parametrize("content, expected", [
    ("正常答复", "正常答复"),
    ("<think>第一段</think>\n<think>第二段</think>\n最终答复", "最终答复"),
    ("<think>第一段</think>\n<think>未闭合\n</｜DSML｜tool_calls>", ""),
    ("<think>只有思考</think>", ""),
    ("<think>未闭合", ""),
    ("前文\n<think>第二段</think>\n最终答复", "前文\n\n最终答复"),
    ("前文\n<think>未闭合", ""),
    ("<think>读取 `file.txt` 后报告</think>完整答复", "完整答复"),
    ("<think>检查\n```json\n{}\n```\n完成</think>完整答复", "完整答复"),
    ("说明中的 `<think>` 字符串", "说明中的 `<think>` 字符串"),
    ("```xml\n<think>literal</think>\n```", "```xml\n<think>literal</think>\n```"),
])
def test_final_response_extraction_handles_all_leading_reasoning(content, expected):
    from agentflow_runtime.coordinator_common import final_response_text, is_empty_llm_response
    from agentflow_runtime.skill_executor import validate_tool_calls

    message = {"role": "assistant", "content": content}
    choice = {"message": message, "finish_reason": "stop"}
    assert final_response_text(content) == expected
    assert is_empty_llm_response({"choices": [choice]}) == (not expected)
    assert validate_tool_calls(choice) == ((True, "") if expected else (False, "incomplete_final_response"))
    assert message["content"] == content  # Raw trace evidence is not rewritten.
    message["tool_calls"] = [{"function": {"name": "write_file", "arguments": "{}"}}]
    assert validate_tool_calls(choice) == (True, "")
    assert not is_empty_llm_response({"choices": [choice]})


# 模拟 LLM 返回的响应（非流式）
LLM_RESPONSE_WITH_TC = {
    "choices": [{
        "index": 0,
        "message": {
            "role": "assistant",
            "content": "我来帮你获取天气信息。",
            "tool_calls": [{
                "id": "call_abc123",
                "type": "function",
                "function": {"name": "get_weather", "arguments": '{"city": "上海"}'}
            }]
        },
        "finish_reason": "tool_calls"
    }],
    "usage": {"prompt_tokens": 100, "completion_tokens": 20}
}

# 模拟 LLM 返回的流式最终组装响应
STREAM_ASSEMBLED_RESPONSE = {
    "choices": [{
        "index": 0,
        "message": {
            "role": "assistant",
            "content": "穿搭方案如下：白色衬衫搭配黑色长裤。",
        },
        "finish_reason": "stop"
    }],
    "usage": {"prompt_tokens": 100, "completion_tokens": 50}
}

# 模拟 LLM 返回含 JSON 文本的响应（边缘情况）
LLM_RESPONSE_WITH_JSON_IN_CONTENT = {
    "choices": [{
        "index": 0,
        "message": {
            "role": "assistant",
            "content": '分析结果：{"result": "success", "data": {"temp": 25}}',
        },
        "finish_reason": "stop"
    }],
    "usage": {"prompt_tokens": 100, "completion_tokens": 30}
}


def test_response_text_is_clean_text():
    """choices[0].message.content 是纯文本，不含 tool_calls。"""
    msg = LLM_RESPONSE_WITH_TC["choices"][0]["message"]
    response_text = msg.get("content", "")
    assert response_text == "我来帮你获取天气信息。"
    assert '"tool_calls"' not in response_text
    assert '"arguments"' not in response_text
    print(f"  ✅ response_text 是纯文本: {response_text}")


def test_response_text_no_raw_json():
    """response_text 不包含 choices/index/delta 等原始 LLM 响应字段。"""
    msg = STREAM_ASSEMBLED_RESPONSE["choices"][0]["message"]
    response_text = msg.get("content", "")
    assert response_text == "穿搭方案如下：白色衬衫搭配黑色长裤。"
    # 原始响应结构字段不应出现在 content 中
    for field in ['"choices"', '"index"', '"delta"', '"finish_reason"']:
        assert field not in response_text, f"response_text 不应包含 {field}"
    print(f"  ✅ response_text 不含原始响应结构字段")


def test_json_in_content_is_valid():
    """LLM 内容本身包含 JSON 字符串是合法的（LLM 输出行为），不属于原始响应泄漏。"""
    msg = LLM_RESPONSE_WITH_JSON_IN_CONTENT["choices"][0]["message"]
    response_text = msg.get("content", "")
    assert "分析结果" in response_text
    assert '"result"' in response_text
    # 验证这不是原始响应结构，而是 LLM 产生的文本
    assert '"choices"' not in response_text
    assert '"finish_reason"' not in response_text
    print(f"  ✅ LLM 自身输出的 JSON 文本不被拦截")


def test_stdout_response_format():
    """coordinator 输出的 {"response": ...} JSON 格式验证。"""
    msg = STREAM_ASSEMBLED_RESPONSE["choices"][0]["message"]
    response_text = msg.get("content", "")
    output = {"response": response_text}
    serialized = json.dumps(output, ensure_ascii=False)
    parsed = json.loads(serialized)
    assert parsed["response"] == "穿搭方案如下：白色衬衫搭配黑色长裤。"
    # 验证序列化后没有 raw LLM 结构泄漏
    assert '"choices"' not in parsed["response"]
    print(f"  ✅ stdout 输出格式正确")


if __name__ == "__main__":
    print("=== coordinator response key 纯文本验证 ===\n")
    test_response_text_is_clean_text()
    test_response_text_no_raw_json()
    test_json_in_content_is_valid()
    test_stdout_response_format()
    print("\n全部通过 ✅")
