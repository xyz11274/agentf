import importlib.util
import subprocess
from pathlib import Path


MODULE_PATH = Path(__file__).parents[1] / "nodes" / "llm_call.py"
SPEC = importlib.util.spec_from_file_location("harness_llm_call", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


def test_curl_failure_preserves_original_stderr(monkeypatch, capsys):
    """curl transport failure keeps the complete native diagnostic.

    Disables transport-layer retry (AF_LLM_MAX_RETRIES=0) so this test stays
    focused on error-message preservation and runs fast — retry behavior is
    covered by test_transport_retry_* below.
    """
    monkeypatch.setenv("AF_LLM_MAX_RETRIES", "0")
    raw_error = (
        "curl: (28) Operation timed out after 120001 milliseconds "
        "with 0 bytes received\nsecond diagnostic line"
    )

    def fake_run(command, **kwargs):
        assert "-sS" in command
        assert "-s" not in command
        return subprocess.CompletedProcess(command, 28, stdout="", stderr=raw_error)

    monkeypatch.setattr(MODULE.subprocess, "run", fake_run)

    result = MODULE.call_llm(
        messages=[{"role": "user", "content": "hello"}],
        model="test-model",
        base_url="http://127.0.0.1:1/v1",
        api_key="test-key",
        timeout=1,
    )

    assert result["_llm_error"] is True
    assert f"curl rc=28: {raw_error}" in result["error"]
    assert raw_error in capsys.readouterr().err


# ── Transport-layer retry invariants (F2: tests must lock invariants) ──


def test_transport_retry_succeeds_after_transient_error(monkeypatch):
    """Transient curl error (rc=35 SSL_ERROR) is retried; success on 2nd attempt.

    Invariant: a single transient network hiccup MUST NOT surface as _llm_error
    to callers — call_llm retries internally and returns the successful response.
    """
    monkeypatch.setenv("AF_LLM_MAX_RETRIES", "2")
    monkeypatch.setenv("AF_LLM_RETRY_BASE_DELAY", "0.01")  # fast test

    call_count = {"n": 0}

    def fake_run(command, **kwargs):
        call_count["n"] += 1
        if call_count["n"] == 1:
            # rc=35 = SSL_ERROR_SYSCALL (the exact error from production logs)
            return subprocess.CompletedProcess(
                command, 35, stdout="",
                stderr="curl: (35) LibreSSL SSL_connect: SSL_ERROR_SYSCALL")
        # 2nd attempt: success
        return subprocess.CompletedProcess(
            command, 0,
            stdout='{"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}],'
                    '"usage":{"prompt_tokens":1,"completion_tokens":1,"total_tokens":2}}',
            stderr="")

    monkeypatch.setattr(MODULE.subprocess, "run", fake_run)

    result = MODULE.call_llm(
        messages=[{"role": "user", "content": "hi"}],
        model="m", base_url="http://x/v1", api_key="k", timeout=1)

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

    def fake_run(command, **kwargs):
        call_count["n"] += 1
        return subprocess.CompletedProcess(
            command, 35, stdout="",
            stderr="curl: (35) LibreSSL SSL_connect: SSL_ERROR_SYSCALL")

    monkeypatch.setattr(MODULE.subprocess, "run", fake_run)

    result = MODULE.call_llm(
        messages=[{"role": "user", "content": "hi"}],
        model="m", base_url="http://x/v1", api_key="k", timeout=1)

    assert result["_llm_error"] is True
    assert "rc=35" in result["error"]
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

    def fake_run(command, **kwargs):
        call_count["n"] += 1
        # HTTP 200 body with API-level error (no "choices") — auth/quota style
        return subprocess.CompletedProcess(
            command, 0,
            stdout='{"error":{"message":"Invalid API key","code":"authentication_error"}}',
            stderr="")

    monkeypatch.setattr(MODULE.subprocess, "run", fake_run)

    result = MODULE.call_llm(
        messages=[{"role": "user", "content": "hi"}],
        model="m", base_url="http://x/v1", api_key="bad", timeout=1)

    assert result["_llm_error"] is True
    assert "API error" in result["error"]
    assert call_count["n"] == 1, "deterministic API error must NOT be retried"


def test_non_transient_curl_error_not_retried(monkeypatch):
    """Non-transient curl error (rc=60 cert failure) is NOT retried.

    Invariant: cert/CA trust errors won't recover on retry — fail fast.
    """
    monkeypatch.setenv("AF_LLM_MAX_RETRIES", "2")
    monkeypatch.setenv("AF_LLM_RETRY_BASE_DELAY", "0.01")

    call_count = {"n": 0}

    def fake_run(command, **kwargs):
        call_count["n"] += 1
        return subprocess.CompletedProcess(
            command, 60, stdout="",
            stderr="curl: (60) Peer certificate cannot be authenticated")

    monkeypatch.setattr(MODULE.subprocess, "run", fake_run)

    result = MODULE.call_llm(
        messages=[{"role": "user", "content": "hi"}],
        model="m", base_url="http://x/v1", api_key="k", timeout=1)

    assert result["_llm_error"] is True
    assert "rc=60" in result["error"]
    assert call_count["n"] == 1, "cert error must NOT be retried"
