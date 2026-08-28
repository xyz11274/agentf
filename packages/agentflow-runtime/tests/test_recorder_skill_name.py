"""LLM recorder skill identity regression tests."""

from __future__ import annotations

import json

import pytest

from agentflow_runtime.recorder import TraceRecorder


@pytest.mark.parametrize(
    "response",
    [
        {
            "choices": [{
                "message": {"role": "assistant", "content": "ok"},
                "finish_reason": "stop",
            }],
            "usage": {
                "prompt_tokens": 1,
                "completion_tokens": 1,
                "total_tokens": 2,
            },
        },
        {"_llm_error": True, "error": "provider unavailable"},
    ],
    ids=["success", "error"],
)
def test_explicit_skill_is_recorded_in_llm_meta(monkeypatch, response) -> None:
    """The dispatch-manifest skill name wins when AF_SKILL_NAME is absent."""
    written = {}
    prepared_events = []

    def capture_hook(name, payload, **kwargs):
        prepared_events.append((name, payload, kwargs))
        return payload


    class FakeHttpClient:
        base_url = "http://llm.invalid"
        last_attempts = 3
        last_retry_reasons = ["http_503", "ConnectionError"]
        last_http_status = 503

        def __init__(self, **_kwargs) -> None:
            self.last_usage_source = "provider" if response.get("usage") else "missing"
            self.last_usage_estimator_version = ""

        def build_request_body(self, messages, **kwargs):
            return {
                "built_by_client": True,
                "messages": messages,
                "stream": kwargs["stream"],
                "extra_body": kwargs.get("extra_body"),
            }

        def chat_completion(self, *_args, **_kwargs):
            assert len(prepared_events) == 1
            return response

    trace_recorder = object.__new__(TraceRecorder)
    trace_recorder._client = object()
    trace_recorder._call_count = 0
    trace_recorder._put = lambda key, value: written.__setitem__(key, value)
    trace_recorder._verify_meta_written = lambda _call_id: None
    trace_recorder._append_to_index = lambda _call_id: None

    monkeypatch.delenv("AF_SKILL_NAME", raising=False)
    monkeypatch.setenv("AF_NODE_ID", "skill_executor")
    monkeypatch.setenv("AF_SELF_TASK_ID", "task-skill-dispatch-1")
    monkeypatch.setenv("AF_EXEC_SKILL_KEY", "dispatch:dialogue:round:1")
    monkeypatch.setenv("AF_BATCH_ID", "batch:dialogue:round")

    from agentflow_runtime.common import clients
    from agentflow_runtime import events, hooks

    monkeypatch.setattr(clients, "OpenAiCompatibleClient", FakeHttpClient)
    monkeypatch.setattr(hooks, "trigger", capture_hook)

    result = trace_recorder.trace_call(
        [{"role": "user", "content": "generate an image"}],
        model="test-model",
        base_url="http://llm.invalid",
        api_key="test-key",
        source="skill_executor",
        skill="forge-image-gen",
        round_i=1,
    )

    meta = next(value for key, value in written.items() if key.endswith("/meta"))
    call_id = result.get("_agentflow_call_id")
    assert call_id
    request = written[f"traces/{call_id}/request"]
    assert len(prepared_events) == 1
    event_name, event_payload, event_kwargs = prepared_events[0]
    assert event_name == events.LLM_REQUEST_PREPARED
    assert event_payload["call_id"] == call_id
    assert event_payload["request"] == request
    assert event_payload["meta"]["skill_name"] == "forge-image-gen"
    assert event_payload["meta"]["request_url"] == "http://llm.invalid"
    assert event_payload["meta"]["connect_timeout_s"] == 5
    assert event_payload["meta"]["request_timeout_s"] == 120
    assert event_kwargs["ctx"].messages == request["messages"]
    assert request["built_by_client"] is True
    assert request["stream"] is False
    provider_response = written[f"traces/{call_id}/provider_response"]
    assert "_agentflow_call_id" not in provider_response
    assert written[f"traces/{call_id}/response"] == provider_response
    assert any(key == f"traces/{call_id}/meta" for key in written)
    assert meta["skill_name"] == "forge-image-gen"
    assert meta["task_type"] == "skill"
    assert meta["task_id"] == "task-skill-dispatch-1"
    assert meta["exec_skill_key"] == "dispatch:dialogue:round:1"
    assert meta["batch_id"] == "batch:dialogue:round"
    assert meta["source"] == "skill_executor"
    assert meta["round"] == 1
    assert meta["transport_attempts"] == 3
    assert meta["retries_used"] == 2
    assert meta["retry_reasons"] == ["http_503", "ConnectionError"]
    assert meta["http_status"] == 503
    assert meta["usage_source"] == ("provider" if response.get("usage") else "missing")
    assert meta["estimator_version"] == ""
    assert meta["request_url"] == "http://llm.invalid"
    assert meta["stream"] is False
    assert meta["connect_timeout_s"] == 5
    assert meta["request_timeout_s"] == 120

    if response.get("_llm_error"):
        assert meta["retries_attempted"] == 2


def test_index_keeps_append_only_entry_for_each_call(monkeypatch) -> None:
    """The authoritative per-call entry is not lost by legacy list races."""
    store = {}

    class FakeNamespace:
        def get(self, key, default=None):
            return store.get(key, default)

        def put(self, key, value):
            store[key] = value
            return True

    class FakeClient:
        session_id = "s1"

        def llm(self):
            return FakeNamespace()

    trace_recorder = object.__new__(TraceRecorder)
    trace_recorder._client = FakeClient()
    monkeypatch.setenv("AF_DIALOGUE_ID", "d1")

    trace_recorder._append_to_index("call-1")

    assert store["traces/index_entries/d1/call-1"] == "call-1"
    assert json.loads(store["traces/index/d1"]) == ["call-1"]


def test_effective_response_preserves_provider_response() -> None:
    """Hook 后响应成为回放真相，同时保留不可变的 Provider 原始响应。"""
    provider_response = {
        "choices": [{
            "message": {
                "role": "assistant",
                "content": "",
                "tool_calls": [{"id": "provider-call"}],
            },
            "finish_reason": "tool_calls",
        }],
        "usage": {"prompt_tokens": 10, "completion_tokens": 2, "total_tokens": 12},
    }
    store = {
        "traces/call-hook/response": provider_response,
        "traces/call-hook/meta": {
            "finish_reason": "tool_calls",
            "response_summary": "provider tool call",
            "total_tokens": 12,
        },
    }

    class FakeNamespace:
        def get(self, key, default=None):
            return store.get(key, default)

        def put(self, key, value):
            store[key] = value
            return True

    class FakeClient:
        session_id = "s1"

        def llm(self):
            return FakeNamespace()

    recorder = object.__new__(TraceRecorder)
    recorder._client = FakeClient()
    effective_response = {
        "choices": [{
            "message": {"role": "assistant", "content": "Hook 后最终回答"},
            "finish_reason": "stop",
        }],
        "usage": {"prompt_tokens": 10, "completion_tokens": 2, "total_tokens": 12},
        "_agentflow_call_id": "call-hook",
    }

    snapshot = recorder.record_effective_response("call-hook", effective_response)
    recorder.record_effective_response("call-hook", effective_response)

    assert "_agentflow_call_id" not in snapshot
    assert store["traces/call-hook/provider_response"] == provider_response
    assert store["traces/call-hook/response"] == snapshot
    meta = store["traces/call-hook/meta"]
    assert meta["provider_finish_reason"] == "tool_calls"
    assert meta["finish_reason"] == "stop"
    assert meta["provider_response_summary"] == "provider tool call"
    assert meta["hook_modified"] is True


def test_session_messages_keep_dialogue_scoped_effective_snapshots(monkeypatch):
    store = {}

    class FakeNamespace:
        def put(self, key, value):
            store[key] = value
            return True

    class FakeClient:
        def llm(self):
            return FakeNamespace()

    recorder = object.__new__(TraceRecorder)
    recorder._client = FakeClient()
    first_messages = [{"role": "user", "content": "first"}]
    second_messages = [{"role": "user", "content": "second"}]

    monkeypatch.setenv("AF_DIALOGUE_ID", "dialogue-1")
    assert recorder.write_session_messages(first_messages) is True
    monkeypatch.setenv("AF_DIALOGUE_ID", "dialogue-2")
    assert recorder.write_session_messages(second_messages) is True

    assert json.loads(
        store["conversations/dialogue-1/effective_messages"]
    ) == first_messages
    assert json.loads(
        store["conversations/dialogue-2/effective_messages"]
    ) == second_messages
    assert json.loads(store["session_messages"]) == second_messages


def test_save_session_messages_emits_snapshot_without_recorder(monkeypatch):
    """Append-only snapshot must not depend on the compatibility cache connection."""
    from agentflow_runtime import events, hooks
    from agentflow_runtime.coordinator_common import save_session_messages

    emitted = []

    def capture(event_name, payload, **_kwargs):
        emitted.append((event_name, payload))
        return payload

    class DisconnectedRecorder:
        is_connected = False

        def write_session_messages(self, _messages):
            raise AssertionError("disconnected compatibility cache must not be written")

    monkeypatch.setattr(hooks, "trigger", capture)
    monkeypatch.setenv("AF_DIALOGUE_ID", "dialogue-snapshot")
    messages = [
        {"role": "system", "content": "system"},
        {"role": "user", "content": "hello"},
    ]

    save_session_messages(DisconnectedRecorder(), messages)

    assert emitted == [(
        events.COORDINATOR_MESSAGES_SNAPSHOT,
        {
            "dialogue_id": "dialogue-snapshot",
            "messages": [{"role": "user", "content": "hello"}],
            "message_count": 1,
        },
    )]
