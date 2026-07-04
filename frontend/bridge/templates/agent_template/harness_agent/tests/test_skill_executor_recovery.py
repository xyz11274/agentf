import importlib.util
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "nodes" / "skill_executor.py"
SPEC = importlib.util.spec_from_file_location("harness_skill_executor", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


def test_length_finish_never_executes_partial_tool_call():
    choice = {
        "finish_reason": "length",
        "message": {
            "tool_calls": [{
                "id": "call-1",
                "function": {"name": "write_file", "arguments": "{}"},
            }]
        },
    }

    valid, reason = MODULE.validate_tool_calls(choice)

    assert not valid
    assert reason == "completion_truncated"


def test_malformed_tool_arguments_are_retried_not_coerced_to_empty_object():
    choice = {
        "finish_reason": "tool_calls",
        "message": {
            "tool_calls": [{
                "id": "call-1",
                "function": {
                    "name": "write_file",
                    "arguments": '{"file_path":"out.html","content":',
                },
            }]
        },
    }

    valid, reason = MODULE.validate_tool_calls(choice)

    assert not valid
    assert reason == "invalid_tool_arguments:write_file"


def test_invalid_tool_turn_is_removed_before_retry():
    messages = [
        {"role": "system", "content": "system"},
        {"role": "user", "content": "task"},
        {
            "role": "assistant",
            "tool_calls": [{
                "id": "broken",
                "function": {"name": "write_file", "arguments": "{"},
            }],
        },
        {"role": "tool", "tool_call_id": "broken", "content": "error"},
    ]

    cleaned = MODULE.remove_invalid_tool_turns(messages)

    assert [message["role"] for message in cleaned] == ["system", "user"]
