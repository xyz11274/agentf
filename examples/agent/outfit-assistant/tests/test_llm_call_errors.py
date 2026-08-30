from __future__ import annotations

import sys
from pathlib import Path

import requests

from agentflow_runtime import llm_call as MODULE

# llm_call.py does `from common.clients import OpenAiCompatibleClient, ...` at
# import time — the ``common`` package lives under ``nodes/``, so we have to put
# ``nodes/`` on sys.path before importing llm_call.py via importlib.
_NODES_DIR = (Path(__file__).parents[1] / "nodes").resolve()
if str(_NODES_DIR) not in sys.path:
    sys.path.insert(0, str(_NODES_DIR))

# After llm_call loads we can import the OpenAI client module — the actual
# ``requests.post`` call happens there, so monkeypatch must target its module
# (not llm_call itself, which no longer imports requests at module top).
from common.clients.openai_compatible_client import (
    OpenAiCompatibleClient,
    requests as occ_requests,
)


# ── Fallback config resolution ──


def test_is_placeholder_empty_string():
    """Empty string is a placeholder."""
    assert MODULE._is_placeholder("") is True


def test_is_placeholder_sk_your_prefix():
    """sk-your- prefix is a placeholder."""
    assert MODULE._is_placeholder("sk-your-api-key") is True


def test_is_placeholder_dollar_brace():
    """${VAR} pattern is a placeholder."""
    assert MODULE._is_placeholder("${AF_LLM_API_KEY}") is True


def test_is_placeholder_real_value():
    """Real model name is not a placeholder."""
    assert MODULE._is_placeholder("gpt-4o") is False
    assert MODULE._is_placeholder("MiniMax-M3") is False


def test_resolve_llm_config_uses_agent_md_model(monkeypatch):
    """agent.md model takes highest priority."""
    monkeypatch.delenv("AF_LLM_MODEL", raising=False)
    monkeypatch.delenv("AF_FALLBACK_LLM_MODEL", raising=False)
    result = MODULE.resolve_llm_config(agent_md_model="gpt-4o")
    assert result["model"] == "gpt-4o"


def test_resolve_llm_config_fallback_to_env(monkeypatch):
    """AF_LLM_MODEL is used when agent.md model is empty."""
    monkeypatch.setenv("AF_LLM_MODEL", "claude-3-opus")
    monkeypatch.delenv("AF_FALLBACK_LLM_MODEL", raising=False)
    result = MODULE.resolve_llm_config(agent_md_model="")
    assert result["model"] == "claude-3-opus"


def test_resolve_llm_config_fallback_to_fallback(monkeypatch):
    """AF_FALLBACK_LLM_MODEL is used when primary and agent.md are empty."""
    monkeypatch.delenv("AF_LLM_MODEL", raising=False)
    monkeypatch.setenv("AF_FALLBACK_LLM_MODEL", "MiniMax-M3")
    result = MODULE.resolve_llm_config(agent_md_model="")
    assert result["model"] == "MiniMax-M3"


def test_resolve_llm_config_fallback_to_hardcoded(monkeypatch):
    """Hardcoded default gpt-4o-mini when nothing is set."""
    monkeypatch.delenv("AF_LLM_MODEL", raising=False)
    monkeypatch.delenv("AF_FALLBACK_LLM_MODEL", raising=False)
    result = MODULE.resolve_llm_config(agent_md_model="")
    assert result["model"] == "gpt-4o-mini"


def test_resolve_llm_config_api_key_direct(monkeypatch):
    """AF_LLM_API_KEY is used directly."""
    monkeypatch.setenv("AF_LLM_API_KEY", "direct-key")
    monkeypatch.delenv("AF_FALLBACK_LLM_API_KEY", raising=False)
    result = MODULE.resolve_llm_config()
    assert result["api_key"] == "direct-key"


def test_resolve_llm_config_api_key_fallback(monkeypatch):
    """AF_FALLBACK_LLM_API_KEY is last resort."""
    monkeypatch.delenv("AF_LLM_API_KEY", raising=False)
    monkeypatch.setenv("AF_FALLBACK_LLM_API_KEY", "ultimate-fallback")
    result = MODULE.resolve_llm_config()
    assert result["api_key"] == "ultimate-fallback"


def test_resolve_llm_config_base_url_chain(monkeypatch):
    """Base URL falls back from AF_LLM_BASE_URL → AF_FALLBACK_LLM_BASE_URL."""
    monkeypatch.delenv("AF_LLM_BASE_URL", raising=False)
    monkeypatch.setenv("AF_FALLBACK_LLM_BASE_URL", "https://fallback.api.com/v1")
    result = MODULE.resolve_llm_config()
    assert result["base_url"] == "https://fallback.api.com/v1"


def test_resolve_vision_config_chain(monkeypatch):
    """Vision config falls back AF_BR_VISION_MODEL → AF_BR_VISION_FALLBACK_MODEL."""
    monkeypatch.delenv("AF_BR_VISION_MODEL", raising=False)
    monkeypatch.setenv("AF_BR_VISION_FALLBACK_MODEL", "qwen-vl-max")
    result = MODULE.resolve_vision_config()
    assert result["model"] == "qwen-vl-max"


def test_resolve_vision_config_api_key_direct(monkeypatch):
    """Vision API key: AF_BR_VISION_API_KEY is used directly."""
    monkeypatch.setenv("AF_BR_VISION_API_KEY", "direct-vision-key")
    monkeypatch.delenv("AF_BR_VISION_FALLBACK_API_KEY", raising=False)
    result = MODULE.resolve_vision_config()
    assert result["api_key"] == "direct-vision-key"


def test_resolve_vision_config_api_key_minimax_fallback(monkeypatch):
    """Vision API key fallback chain: AF_BR_VISION_API_KEY → MINIMAX_API_KEY → AF_BR_VISION_FALLBACK_API_KEY.

    Contract: when higher-priority keys are unset, the next key in the chain
    is used. This test verifies the middle link (MINIMAX_API_KEY) is hit
    when AF_BR_VISION_API_KEY is unset, regardless of the specific vendor.

    All three env vars must be cleared first to avoid real-environment leakage.
    """
    monkeypatch.delenv("AF_BR_VISION_API_KEY", raising=False)
    monkeypatch.delenv("MINIMAX_API_KEY", raising=False)
    monkeypatch.delenv("AF_BR_VISION_FALLBACK_API_KEY", raising=False)
    # Set the middle-priority key; expect it to be returned
    monkeypatch.setenv("MINIMAX_API_KEY", "minimax-key")
    result = MODULE.resolve_vision_config()
    assert result["api_key"] == "minimax-key"


def test_resolve_llm_config_placeholder_in_model_skips_to_fallback(monkeypatch):
    """Placeholder model name skips to next priority level."""
    monkeypatch.delenv("AF_LLM_MODEL", raising=False)
    monkeypatch.setenv("AF_FALLBACK_LLM_MODEL", "real-model")
    # agent.md has a placeholder model
    result = MODULE.resolve_llm_config(agent_md_model="${MODEL_NAME}")
    assert result["model"] == "real-model"


class _MockResponse:
    """Minimal requests.Response stand-in with a .json() method."""

    def __init__(self, json_data: dict, status_code: int = 200):
        self._json_data = json_data
        self.status_code = status_code

    def json(self):
        return self._json_data


def test_http_timeout_preserves_exception(monkeypatch, capsys):
    """HTTP transport failure (timeout) keeps the complete exception diagnostic.

    Disables transport-layer retry (AF_LLM_MAX_RETRIES=0) so this test stays
    focused on error-message preservation and runs fast — retry behavior is
    covered by test_transport_retry_* below.
    """
    monkeypatch.setenv("AF_LLM_MAX_RETRIES", "0")

    def fake_post(*args, **kwargs):
        raise requests.exceptions.Timeout(
            "Connection timed out after 5000 milliseconds with 0 bytes received"
        )

    monkeypatch.setattr(occ_requests, "post", fake_post)

    result = MODULE.call_llm(
        messages=[{"role": "user", "content": "hello"}],
        model="test-model",
        base_url="http://127.0.0.1:1/v1",
        api_key="test-key",
        timeout=1,
    )

    assert result["_llm_error"] is True
    assert "Connection timed out" in result["error"]
    assert "Connection timed out" in capsys.readouterr().err


# ── Transport-layer retry invariants (F2: tests must lock invariants) ──


def test_transport_retry_succeeds_after_transient_error(monkeypatch):
    """Transient ConnectionError is retried; success on 2nd attempt.

    Invariant: a single transient network hiccup MUST NOT surface as _llm_error
    to callers — call_llm retries internally and returns the successful response.
    """
    monkeypatch.setenv("AF_LLM_MAX_RETRIES", "2")
    monkeypatch.setenv("AF_LLM_RETRY_BASE_DELAY", "0.01")  # fast test

    call_count = {"n": 0}

    def fake_post(*args, **kwargs):
        call_count["n"] += 1
        if call_count["n"] == 1:
            raise requests.exceptions.ConnectionError(
                "Connection reset by peer")
        # 2nd attempt: success
        return _MockResponse({
            "choices": [{"message": {"content": "ok"}, "finish_reason": "stop"}],
            "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
        })

    monkeypatch.setattr(occ_requests, "post", fake_post)

    result = MODULE.call_llm(
        messages=[{"role": "user", "content": "hi"}],
        model="m", base_url="http://127.0.0.1:9/v1", api_key="k", timeout=1)

    assert "_llm_error" not in result, f"should have retried, got: {result}"
    assert result["choices"][0]["message"]["content"] == "ok"
    assert call_count["n"] == 2, "must have retried exactly once after transient error"


def test_transport_retry_exhausted_returns_llm_error(monkeypatch):
    """Persistent transient error exhausts retries → _llm_error returned.

    Invariant: when ALL retries fail with the same transient error, call_llm
    gives up and returns _llm_error (callers' fallback path triggers).
    """
    monkeypatch.setenv("AF_LLM_MAX_RETRIES", "2")
    monkeypatch.setenv("AF_LLM_RETRY_BASE_DELAY", "0.01")

    call_count = {"n": 0}

    def fake_post(*args, **kwargs):
        call_count["n"] += 1
        raise requests.exceptions.ConnectionError(
            "Connection reset by peer")

    monkeypatch.setattr(occ_requests, "post", fake_post)

    result = MODULE.call_llm(
        messages=[{"role": "user", "content": "hi"}],
        model="m", base_url="http://127.0.0.1:9/v1", api_key="k", timeout=1)

    assert result["_llm_error"] is True
    assert "Connection reset" in result["error"]
    # 1 initial + 2 retries = 3 total attempts
    assert call_count["n"] == 3, f"expected 3 attempts, got {call_count['n']}"


def test_deterministic_api_error_not_retried(monkeypatch):
    """Business API error (auth/400/quota) is NOT retried.

    Invariant: deterministic errors waste time on retry — must fail fast.
    Uses a 401-style error body (HTTP 200 with JSON {"error": ...}).
    """
    monkeypatch.setenv("AF_LLM_MAX_RETRIES", "2")
    monkeypatch.setenv("AF_LLM_RETRY_BASE_DELAY", "0.01")

    call_count = {"n": 0}

    def fake_post(*args, **kwargs):
        call_count["n"] += 1
        # HTTP 200 body with API-level error (no "choices") — auth/quota style
        return _MockResponse({
            "error": {"message": "Invalid API key", "code": "authentication_error"},
        })

    monkeypatch.setattr(occ_requests, "post", fake_post)

    result = MODULE.call_llm(
        messages=[{"role": "user", "content": "hi"}],
        model="m", base_url="http://127.0.0.1:9/v1", api_key="bad", timeout=1)

    assert result["_llm_error"] is True
    assert "API error" in result["error"]
    assert call_count["n"] == 1, "deterministic API error must NOT be retried"


def test_non_transient_ssl_error_not_retried(monkeypatch):
    """SSLError (cert trust) is NOT retried.

    Invariant: cert/CA trust errors won't recover on retry — fail fast.
    Equivalent to old curl rc=60 behavior.
    """
    monkeypatch.setenv("AF_LLM_MAX_RETRIES", "2")
    monkeypatch.setenv("AF_LLM_RETRY_BASE_DELAY", "0.01")

    call_count = {"n": 0}

    def fake_post(*args, **kwargs):
        call_count["n"] += 1
        raise requests.exceptions.SSLError(
            "certificate verify failed: self-signed certificate")

    monkeypatch.setattr(occ_requests, "post", fake_post)

    result = MODULE.call_llm(
        messages=[{"role": "user", "content": "hi"}],
        model="m", base_url="http://127.0.0.1:9/v1", api_key="k", timeout=1)

    assert result["_llm_error"] is True
    assert "certificate verify failed" in result["error"]
    assert call_count["n"] == 1, "SSL error must NOT be retried"
