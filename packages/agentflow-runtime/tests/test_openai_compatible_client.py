"""OpenAI-compatible client total-deadline regression tests."""

import json

import requests
import pytest

from agentflow_runtime.common.clients import openai_compatible_client as client_module


class _FakeResponse:
    status_code = 200

    def json(self):
        return {
            "choices": [{
                "message": {"role": "assistant", "content": "ok"},
                "finish_reason": "stop",
            }],
            "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
        }


class _ResponseWithBody:
    status_code = 200

    def __init__(self, body):
        self._body = body

    def json(self):
        return self._body


class _FakeSseResponse:
    status_code = 200

    def __init__(self, chunks):
        self._chunks = chunks

    def iter_lines(self, decode_unicode=True):
        for chunk in self._chunks:
            yield f"data: {json.dumps(chunk)}"
        yield "data: [DONE]"


class _FakeTime:
    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


def test_retries_share_one_request_timeout_budget(monkeypatch):
    """每次尝试只获得总预算中按剩余尝试次数分配的份额。"""
    fake_time = _FakeTime()
    calls = []

    def fake_post(*args, **kwargs):
        calls.append(kwargs)
        fake_time.now += 120.0
        raise requests.Timeout("simulated read timeout")

    monkeypatch.delenv("AF_LLM_TIMEOUT", raising=False)
    monkeypatch.delenv("AF_LLM_MAX_RETRIES", raising=False)
    monkeypatch.setattr(client_module, "time", fake_time)
    monkeypatch.setattr(client_module.requests, "post", fake_post)

    client = client_module.OpenAiCompatibleClient(
        model="test-model",
        base_url="https://llm.test/v1",
        api_key="test",
        max_retries=2,
        retry_base_delay=1.0,
        request_timeout=120,
    )
    result = client.chat_completion([{"role": "user", "content": "hello"}])

    assert result["_llm_error"] is True
    assert "total timeout exhausted" in result["error"]
    assert len(calls) == 1
    assert calls[0]["timeout"] == (5.0, 40.0)
    assert client.last_attempts == 1


def test_reports_actual_transport_attempts_after_retries(monkeypatch):
    calls = []

    def fake_post(*args, **kwargs):
        calls.append(kwargs)
        if len(calls) < 3:
            raise requests.ConnectionError("temporary connection reset")
        return _FakeResponse()

    monkeypatch.delenv("AF_LLM_TIMEOUT", raising=False)
    monkeypatch.delenv("AF_LLM_MAX_RETRIES", raising=False)
    monkeypatch.setattr(client_module.requests, "post", fake_post)
    monkeypatch.setattr(client_module.time, "sleep", lambda _seconds: None)

    client = client_module.OpenAiCompatibleClient(
        model="test-model",
        base_url="https://llm.test/v1",
        api_key="test",
        max_retries=2,
        retry_base_delay=0.0,
        request_timeout=120,
    )
    result = client.chat_completion([{"role": "user", "content": "hello"}])

    assert result["choices"][0]["message"]["content"] == "ok"
    assert len(calls) == 3
    assert client.last_attempts == 3


@pytest.mark.parametrize("body", [
    [],
    {},
    {"choices": []},
    {"choices": [{}]},
    {"choices": [{"message": "not-an-object"}]},
    {
        "choices": [{"message": {"role": "assistant", "content": "ok"}}],
        "usage": "not-an-object",
    },
])
def test_malformed_non_stream_response_becomes_llm_error(monkeypatch, body):
    """Provider 畸形 JSON 应进入统一错误协议，而不是在 Recorder 中索引崩溃。"""
    monkeypatch.delenv("AF_LLM_TIMEOUT", raising=False)
    monkeypatch.delenv("AF_LLM_MAX_RETRIES", raising=False)
    monkeypatch.setattr(
        client_module.requests,
        "post",
        lambda *_args, **_kwargs: _ResponseWithBody(body),
    )

    client = client_module.OpenAiCompatibleClient(
        model="test-model",
        base_url="https://llm.test/v1",
        api_key="test",
        request_timeout=120,
    )
    result = client.chat_completion([{"role": "user", "content": "hello"}])

    assert result["_llm_error"] is True
    assert "Malformed chat completion response" in result["error"]
    assert client.last_attempts == 1


class _ProviderErrorResponse:
    def __init__(self, status_code):
        self.status_code = status_code
        self.text = f"provider status {status_code}"

    def json(self):
        return {"error": {"message": self.text}}


@pytest.mark.parametrize("status_code", [408, 429, 500, 502, 503, 504])
def test_retryable_provider_status_retries_then_succeeds(monkeypatch, status_code):
    responses = iter([_ProviderErrorResponse(status_code), _FakeResponse()])
    calls = []

    def fake_post(*_args, **kwargs):
        calls.append(kwargs)
        return next(responses)

    monkeypatch.delenv("AF_LLM_TIMEOUT", raising=False)
    monkeypatch.delenv("AF_LLM_MAX_RETRIES", raising=False)
    monkeypatch.setattr(client_module.requests, "post", fake_post)
    monkeypatch.setattr(client_module.time, "sleep", lambda _seconds: None)

    client = client_module.OpenAiCompatibleClient(
        model="test-model",
        base_url="https://llm.test/v1",
        api_key="test",
        max_retries=1,
        retry_base_delay=0.0,
        request_timeout=120,
    )
    result = client.chat_completion([{"role": "user", "content": "hello"}])

    assert result["choices"][0]["message"]["content"] == "ok"
    assert len(calls) == 2
    assert client.last_attempts == 2
    assert client.last_retry_reasons == [f"http_{status_code}"]
    assert client.last_http_status == 200


@pytest.mark.parametrize("status_code", [400, 401, 501])
def test_deterministic_provider_status_is_not_retried(monkeypatch, status_code):
    calls = []

    def fake_post(*_args, **kwargs):
        calls.append(kwargs)
        return _ProviderErrorResponse(status_code)

    monkeypatch.delenv("AF_LLM_TIMEOUT", raising=False)
    monkeypatch.delenv("AF_LLM_MAX_RETRIES", raising=False)
    monkeypatch.setattr(client_module.requests, "post", fake_post)
    monkeypatch.setattr(client_module.time, "sleep", lambda _seconds: None)

    client = client_module.OpenAiCompatibleClient(
        model="test-model",
        base_url="https://llm.test/v1",
        api_key="test",
        max_retries=2,
        retry_base_delay=0.0,
        request_timeout=120,
    )
    result = client.chat_completion([{"role": "user", "content": "hello"}])

    assert result["_llm_error"] is True
    assert f"HTTP {status_code}" in result["error"]
    assert len(calls) == 1
    assert client.last_attempts == 1
    assert client.last_retry_reasons == []
    assert client.last_http_status == status_code


def test_stream_usage_is_marked_estimated(monkeypatch):
    fake_response = _FakeSseResponse([
        {"choices": [{"delta": {"content": "estimated"}, "index": 0}]},
    ])
    monkeypatch.delenv("AF_LLM_TIMEOUT", raising=False)
    monkeypatch.delenv("AF_LLM_MAX_RETRIES", raising=False)
    monkeypatch.setattr(
        client_module.requests, "post", lambda *_args, **_kwargs: fake_response)

    client = client_module.OpenAiCompatibleClient(
        model="test-model",
        base_url="https://llm.test/v1",
        api_key="test",
        request_timeout=120,
    )
    result = client.chat_completion(
        [{"role": "user", "content": "hello"}],
        stream=True,
    )

    assert result["usage"]["prompt_tokens"] == 0
    assert client.last_usage_source == "estimated"
    assert client.last_usage_estimator_version == "chars_div_2_v1"


def test_stream_provider_usage_chunk_is_preserved(monkeypatch):
    provider_usage = {
        "prompt_tokens": 9,
        "completion_tokens": 2,
        "total_tokens": 11,
    }
    fake_response = _FakeSseResponse([
        {"choices": [{"delta": {"content": "ok"}, "index": 0}]},
        {"choices": [], "usage": provider_usage},
    ])
    monkeypatch.delenv("AF_LLM_TIMEOUT", raising=False)
    monkeypatch.delenv("AF_LLM_MAX_RETRIES", raising=False)
    monkeypatch.setattr(
        client_module.requests, "post", lambda *_args, **_kwargs: fake_response)

    client = client_module.OpenAiCompatibleClient(
        model="test-model",
        base_url="https://llm.test/v1",
        api_key="test",
        request_timeout=120,
    )
    result = client.chat_completion(
        [{"role": "user", "content": "hello"}],
        stream=True,
    )

    assert result["usage"] == provider_usage
    assert client.last_usage_source == "provider"
    assert client.last_usage_estimator_version == ""


def test_request_body_builder_matches_transport_json(monkeypatch):
    """Recorder 和 HTTP transport 必须共享同一份请求体构造语义。"""
    captured = {}

    def fake_post(*_args, **kwargs):
        captured["json"] = kwargs["json"]
        return _FakeSseResponse([
            {"choices": [{"delta": {"content": "ok"}, "index": 0}]},
        ])

    monkeypatch.delenv("AF_LLM_TIMEOUT", raising=False)
    monkeypatch.delenv("AF_LLM_MAX_RETRIES", raising=False)
    monkeypatch.setattr(client_module.requests, "post", fake_post)

    client = client_module.OpenAiCompatibleClient(
        model="aisf/test-model",
        base_url="https://llm.test/v1",
        api_key="test",
        request_timeout=120,
    )
    messages = [{"role": "user", "content": "hello"}]
    tools = [{"type": "function", "function": {"name": "lookup"}}]
    kwargs = {
        "tools": tools,
        "max_completion_tokens": 321,
        "stream": True,
        "extra_body": {
            "temperature": 0.2,
            "top_p": 0.8,
            "stop": None,
            "model": "must-not-override",
            "stream": False,
            "enable_thinking": False,
        },
    }

    expected = client.build_request_body(messages, **kwargs)
    result = client.chat_completion(messages, **kwargs)

    assert result["choices"][0]["message"]["content"] == "ok"
    assert captured["json"] == expected
    assert expected == {
        "model": "test-model",
        "messages": messages,
        "max_completion_tokens": 321,
        "tools": tools,
        "tool_choice": "auto",
        "stream": True,
        "enable_thinking": True,
        "temperature": 0.2,
        "top_p": 0.8,
    }

def test_rejects_credential_bearing_remote_http_endpoint():
    with pytest.raises(ValueError, match="must use HTTPS"):
        client_module.OpenAiCompatibleClient(
            model="test-model",
            base_url="http://provider.example/v1",
            api_key="transport-secret-998877",
        )


def test_transport_enforces_tls_no_redirects_and_no_ambient_proxy(monkeypatch):
    calls = []

    def fake_post(*args, **kwargs):
        calls.append((args, kwargs))
        return _FakeResponse()

    monkeypatch.delenv("AF_LLM_TIMEOUT", raising=False)
    monkeypatch.delenv("AF_LLM_MAX_RETRIES", raising=False)
    monkeypatch.setattr(client_module.requests, "post", fake_post)
    client = client_module.OpenAiCompatibleClient(
        model="test-model",
        base_url="https://provider.example/v1",
        api_key="transport-secret-998877",
    )

    result = client.chat_completion([{"role": "user", "content": "hello"}])

    assert result["choices"][0]["message"]["content"] == "ok"
    kwargs = calls[0][1]
    assert kwargs["verify"] is True
    assert kwargs["allow_redirects"] is False
    assert kwargs["proxies"] == {"http": "", "https": ""}


def test_registered_secret_in_request_is_blocked_before_network(monkeypatch):
    secret = "request-secret-998877665544"
    called = False

    def fake_post(*_args, **_kwargs):
        nonlocal called
        called = True
        return _FakeResponse()

    monkeypatch.setattr(client_module.requests, "post", fake_post)
    client = client_module.OpenAiCompatibleClient(
        model="test-model",
        base_url="https://provider.example/v1",
        api_key=secret,
    )

    result = client.chat_completion([
        {"role": "user", "content": f"please repeat {secret}"},
    ])

    assert result["_llm_error"] is True
    assert result["error"] == "LLM request blocked by credential isolation policy"
    assert result["security"]["applied"] is True
    assert secret not in json.dumps(result)
    assert called is False
    assert client.last_attempts == 0


def test_provider_response_and_error_cannot_echo_registered_secret(monkeypatch):
    secret = "provider-response-secret-998877"
    responses = iter([
        _ResponseWithBody({
            "choices": [{
                "message": {"role": "assistant", "content": f"echo {secret}"},
                "finish_reason": "stop",
            }],
            "usage": {},
        }),
        _ProviderErrorResponse(401),
    ])

    def fake_post(*_args, **_kwargs):
        response = next(responses)
        if isinstance(response, _ProviderErrorResponse):
            response.text = f"provider rejected {secret}"
        return response

    monkeypatch.setattr(client_module.requests, "post", fake_post)
    client = client_module.OpenAiCompatibleClient(
        model="test-model",
        base_url="https://provider.example/v1",
        api_key=secret,
    )

    response = client.chat_completion([{"role": "user", "content": "hello"}])
    error = client.chat_completion([{"role": "user", "content": "hello again"}])

    assert secret not in json.dumps(response)
    assert "<redacted:llm.api_key>" in response["choices"][0]["message"]["content"]
    assert secret not in json.dumps(error)
    assert "<redacted:llm.api_key>" in error["error"]


def test_stream_callback_cannot_reassemble_secret_split_across_deltas(monkeypatch):
    secret = "provider-stream-secret-998877665544"
    fake_response = _FakeSseResponse([
        {"choices": [{"delta": {"content": "safe:"}, "index": 0}]},
        {"choices": [{"delta": {"content": secret[:9]}, "index": 0}]},
        {"choices": [{"delta": {"content": secret[9:21]}, "index": 0}]},
        {"choices": [{"delta": {"content": secret[21:]}, "index": 0}]},
        {"choices": [{"delta": {"content": ":done"}, "index": 0}]},
    ])
    monkeypatch.setattr(
        client_module.requests, "post", lambda *_args, **_kwargs: fake_response,
    )
    client = client_module.OpenAiCompatibleClient(
        model="test-model",
        base_url="https://provider.example/v1",
        api_key=secret,
    )
    callback_payloads = []

    result = client.chat_completion(
        [{"role": "user", "content": "hello"}],
        stream=True,
        on_chunk=callback_payloads.append,
    )

    callback_text = "".join(
        json.loads(payload)["choices"][0]["delta"].get("content", "")
        for payload in callback_payloads
    )
    assert secret not in callback_text
    assert "<redacted:llm.api_key>" in callback_text
    assert secret not in json.dumps(result)

def test_stream_callback_cannot_reassemble_secret_split_across_tool_arguments(
    monkeypatch,
):
    secret = "provider-tool-secret-998877665544"
    fake_response = _FakeSseResponse([
        {
            "choices": [{
                "delta": {
                    "tool_calls": [{
                        "index": 0,
                        "id": "call-1",
                        "type": "function",
                        "function": {
                            "name": "example_tool",
                            "arguments": secret[:8],
                        },
                    }],
                },
                "index": 0,
            }],
        },
        {
            "choices": [{
                "delta": {
                    "tool_calls": [{
                        "index": 0,
                        "function": {"arguments": secret[8:20]},
                    }],
                },
                "index": 0,
            }],
        },
        {
            "choices": [{
                "delta": {
                    "tool_calls": [{
                        "index": 0,
                        "function": {"arguments": secret[20:]},
                    }],
                },
                "index": 0,
            }],
        },
    ])
    monkeypatch.setattr(
        client_module.requests, "post", lambda *_args, **_kwargs: fake_response,
    )
    client = client_module.OpenAiCompatibleClient(
        model="test-model",
        base_url="https://provider.example/v1",
        api_key=secret,
    )
    callback_payloads = []

    result = client.chat_completion(
        [{"role": "user", "content": "hello"}],
        stream=True,
        on_chunk=callback_payloads.append,
    )

    callback_arguments = []
    for payload in callback_payloads:
        delta = json.loads(payload)["choices"][0]["delta"]
        for tool_call in delta.get("tool_calls", []):
            callback_arguments.append(
                (tool_call.get("function") or {}).get("arguments", "")
            )
    callback_text = "".join(callback_arguments)
    assert secret not in callback_text
    assert "<redacted:llm.api_key>" in callback_text
    assert secret not in json.dumps(result)
