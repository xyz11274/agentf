#!/usr/bin/env python3
"""Layer 1 unit/integration tests for outfit_vision.py timeout fix + concurrent log isolation.

Tests the fix that removed the _urlopen retry-on-URLError anti-pattern (was 120s×2=240s)
and replaced it with a single-shot urlopen + 30s timeout + no retry on network errors.

Endpoint Injection Strategy: **Option B** (env var hook).
  The production script reads `OUTFIT_VISION_ENDPOINT` env var (1-line change to outfit_vision.py).
  - In-process tests (T1.1, T1.2, T1.4): import outfit_vision and monkeypatch the ENDPOINT
    attribute directly.
  - Concurrency test (T1.3): uses subprocess.run with env var injection for full process
    isolation (preferred over ProcessPoolExecutor to avoid macOS 'spawn' re-import fragility
    with pytest fixtures).

Invariants verified:
  I4: outfit_vision HTTP error returns within ≤30s (NOT 240s)
  I5: On failure: stderr contains error message AND /tmp/outfit_vision.log contains structured warning
  I6: 5 concurrent invocations: all independent, log file entries distinguishable, no contention

Run:
  pytest frontend/bridge/tests/test_outfit_vision_concurrent.py -v --timeout=120
"""

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

# ── Path setup ──────────────────────────────────────────────────────

_PROJECT_ROOT = Path(__file__).resolve().parents[3]
_SCRIPTS_DIR = _PROJECT_ROOT / "examples" / "agent" / "outfit-assistant" / "harness" / "skills" / "outfit-stylist" / "scripts"
_SCRIPT_PATH = _SCRIPTS_DIR / "outfit_vision.py"
_LOG_FILE = "/tmp/outfit_vision.log"

# Add scripts dir to sys.path so we can import outfit_vision as a module
if str(_SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS_DIR))

# Test image: a URL passes through _image_url without any file I/O
_TEST_IMAGE = "https://example.com/test_outfit.jpg"


# ── Mock HTTP Server Handlers ───────────────────────────────────────

class _SlowHandler(BaseHTTPRequestHandler):
    """Sleeps 60s before responding — triggers the 30s socket timeout in the client."""

    protocol_version = "HTTP/1.1"

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("content-length", "0"))
        self.rfile.read(length)
        time.sleep(60)  # client will have timed out by now
        try:
            self.send_response(200)
            self.send_header("Content-Length", "0")
            self.end_headers()
        except Exception:
            pass  # client disconnected — expected

    def log_message(self, format, *args) -> None:  # noqa: A002
        pass  # suppress stderr noise


class _NonJsonContentHandler(BaseHTTPRequestHandler):
    """Returns valid OpenAI-format JSON but with non-JSON-parseable content.

    Exercises _extract_json's {"raw": ...} fallback path.
    """

    protocol_version = "HTTP/1.1"

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("content-length", "0"))
        self.rfile.read(length)
        payload = json.dumps({
            "id": "chatcmpl-test",
            "object": "chat.completion",
            "created": 1,
            "model": "test-model",
            "choices": [{
                "index": 0,
                "message": {"role": "assistant", "content": "not a json"},
                "finish_reason": "stop",
            }],
            "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
        }).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format, *args) -> None:  # noqa: A002
        pass


class _Http500Handler(BaseHTTPRequestHandler):
    """Returns HTTP 500 — triggers HTTPError path."""

    protocol_version = "HTTP/1.1"

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("content-length", "0"))
        self.rfile.read(length)
        body = b'{"error": "internal server error"}'
        self.send_response(500)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args) -> None:  # noqa: A002
        pass


def _start_mock_server(handler_class: type[BaseHTTPRequestHandler]) -> ThreadingHTTPServer:
    """Start a ThreadingHTTPServer on a random port (port 0), return the server."""
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler_class)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


def _stop_mock_server(server: ThreadingHTTPServer) -> None:
    server.shutdown()
    server.server_close()


def _get_closed_port() -> int:
    """Bind to port 0, get the assigned port, close the socket. Returns a now-closed port."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def _disable_proxies(monkeypatch):
    """Clear proxy env vars so urllib connects directly to localhost."""
    monkeypatch.setenv("NO_PROXY", "127.0.0.1,localhost,*")
    monkeypatch.setenv("no_proxy", "127.0.0.1,localhost,*")
    for var in ("HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy",
                "ALL_PROXY", "all_proxy"):
        monkeypatch.delenv(var, raising=False)


# ── Fixtures ────────────────────────────────────────────────────────

@pytest.fixture(autouse=True)
def _clean_log_file():
    """Remove /tmp/outfit_vision.log before and after each test to avoid cross-test pollution."""
    if os.path.exists(_LOG_FILE):
        os.remove(_LOG_FILE)
    yield
    if os.path.exists(_LOG_FILE):
        os.remove(_LOG_FILE)


@pytest.fixture
def mock_slow_server():
    """Mock server that sleeps 60s, triggering 30s socket timeout (I4)."""
    server = _start_mock_server(_SlowHandler)
    port = server.server_address[1]
    yield f"http://127.0.0.1:{port}/v1/chat/completions"
    _stop_mock_server(server)


@pytest.fixture
def mock_non_json_server():
    """Mock server returning valid JSON with non-JSON-parseable content field."""
    server = _start_mock_server(_NonJsonContentHandler)
    port = server.server_address[1]
    yield f"http://127.0.0.1:{port}/v1/chat/completions"
    _stop_mock_server(server)


@pytest.fixture
def mock_http_500_server():
    """Mock server returning HTTP 500."""
    server = _start_mock_server(_Http500Handler)
    port = server.server_address[1]
    yield f"http://127.0.0.1:{port}/v1/chat/completions"
    _stop_mock_server(server)


# ── Helper: run outfit_vision.main() in-process ────────────────────

def _run_main_inprocess(endpoint_url: str, capsys=None) -> tuple[int, str, str]:
    """Import outfit_vision, patch ENDPOINT, call main(), return (rc, stdout, stderr).

    Requires DASHSCOPE_API_KEY to be set for the HTTP path.
    """
    os.environ.setdefault("DASHSCOPE_API_KEY", "test-key-for-mock")

    import outfit_vision  # type: ignore[import-not-found]
    original_endpoint = outfit_vision.ENDPOINT
    outfit_vision.ENDPOINT = endpoint_url
    try:
        if capsys:
            capsys.readouterr()  # clear prior output
        rc = outfit_vision.main(["--image", _TEST_IMAGE, "--model", "qwen-vl-max"])
        if capsys:
            captured = capsys.readouterr()
            return rc, captured.out, captured.err
        return rc, "", ""
    finally:
        outfit_vision.ENDPOINT = original_endpoint


# ═══════════════════════════════════════════════════════════════════
# T1.1: Slow server triggers 30s timeout (NOT 240s)
# ═══════════════════════════════════════════════════════════════════

def test_http_slow_server_fails_within_timeout(mock_slow_server, capsys, monkeypatch):
    """T1.1 / I4: Mock server sleeps 60s; outfit_vision must time out within ≤30s.

    Before the fix, _urlopen retried on URLError (120s × 2 = 240s).
    After the fix: single-shot urlopen with 30s timeout → URLError after ~30s.
    """
    monkeypatch.setenv("DASHSCOPE_API_KEY", "test-key-for-mock")
    _disable_proxies(monkeypatch)

    t0 = time.monotonic()
    rc, stdout, stderr = _run_main_inprocess(mock_slow_server, capsys)
    elapsed = time.monotonic() - t0

    # I4: must fail within 35s (30s timeout + 5s buffer)
    assert elapsed < 35, f"wall-clock {elapsed:.1f}s — expected < 35s (30s timeout + buffer). " \
                         f"If ~240s, the retry anti-pattern is back."
    assert rc != 0, f"expected non-zero return for timeout, got {rc}"


# ═══════════════════════════════════════════════════════════════════
# T1.2: Connection refused emits structured error to stderr + log file
# ═══════════════════════════════════════════════════════════════════

def test_connection_refused_emits_structured_error(capsys, monkeypatch):
    """T1.2 / I5: Pointing ENDPOINT to a closed port → URLError.

    Asserts:
      - return == 1
      - stderr contains 'network error calling DashScope'
      - /tmp/outfit_vision.log contains 'URLError' or 'network error' in recent lines
    """
    monkeypatch.setenv("DASHSCOPE_API_KEY", "test-key-for-mock")
    _disable_proxies(monkeypatch)

    closed_port = _get_closed_port()
    closed_endpoint = f"http://127.0.0.1:{closed_port}/v1/chat/completions"

    rc, stdout, stderr = _run_main_inprocess(closed_endpoint, capsys)

    assert rc == 1, f"expected return 1 for connection refused, got {rc}"
    assert "network error calling DashScope" in stderr, \
        f"stderr should contain structured error message, got: {stderr!r}"

    # I5: log file must contain structured warning
    assert os.path.exists(_LOG_FILE), "log file should exist after error"
    with open(_LOG_FILE) as f:
        log_content = f.read()
    recent_log = log_content[-500:] if len(log_content) > 500 else log_content
    assert "URLError" in recent_log or "network error" in recent_log, \
        f"log file should contain 'URLError' or 'network error', got last 500 chars: {recent_log!r}"


# ═══════════════════════════════════════════════════════════════════
# T1.3: 5 concurrent invocations are independent (THE KEY CONCURRENCY TEST)
# ═══════════════════════════════════════════════════════════════════

def test_concurrent_5_invocations_are_independent(mock_slow_server):
    """T1.3 / I6: 5 concurrent invocations against a slow endpoint.

    Each invocation hits the 30s socket timeout independently.
    Uses subprocess.run for full process isolation (Option B env var injection).

    Concurrency proof: 5 × 30s serial = 150s; parallel ≈ 30s. Assert total < 60s.

    Implementation choice: subprocess + ThreadPoolExecutor (NOT ProcessPoolExecutor).
    Rationale: ProcessPoolExecutor on macOS uses 'spawn', which re-imports the test
    module in each child process — fragile with pytest fixtures. subprocess.run with
    env var gives true process isolation without pickling concerns.
    """
    env = dict(os.environ)
    env["OUTFIT_VISION_ENDPOINT"] = mock_slow_server
    env["DASHSCOPE_API_KEY"] = "test-key-for-mock"
    # Ensure the mock HTTP (not HTTPS) proxy isn't interfering
    env["NO_PROXY"] = "127.0.0.1,localhost"
    env["no_proxy"] = "127.0.0.1,localhost"

    def _run_one(idx: int) -> tuple[int, float, str]:
        """Run outfit_vision.py as a subprocess, return (rc, wall_clock, stderr)."""
        t0 = time.monotonic()
        result = subprocess.run(
            [sys.executable, str(_SCRIPT_PATH),
             "--image", _TEST_IMAGE, "--model", "qwen-vl-max"],
            env=env,
            capture_output=True,
            text=True,
            timeout=40,  # safety net: 30s script timeout + 10s overhead
        )
        elapsed = time.monotonic() - t0
        return (result.returncode, elapsed, result.stderr)

    t0_total = time.monotonic()
    with ThreadPoolExecutor(max_workers=5) as executor:
        futures = [executor.submit(_run_one, i) for i in range(5)]
        results = [f.result() for f in futures]
    total_elapsed = time.monotonic() - t0_total

    # Per-invocation assertions
    for i, (rc, elapsed, stderr) in enumerate(results):
        assert rc != 0, \
            f"worker {i}: expected non-zero return (timeout), got {rc}. stderr: {stderr!r}"
        assert elapsed < 35, \
            f"worker {i}: wall-clock {elapsed:.1f}s — expected < 35s (30s timeout + buffer)"

    # CONCURRENCY PROOF: total wall-clock must be < 60s
    # Serial execution would be 5 × 30s = 150s.
    # If we see ~30s, invocations ran in parallel.
    # < 60s gives 2× margin while still proving parallelism (serial would be 150s).
    assert total_elapsed < 60, \
        f"total wall-clock {total_elapsed:.1f}s — expected < 60s to prove parallel execution " \
        f"(serial would be ~150s). Per-worker: {[(round(e, 1)) for _, e, _ in results]}"

    # I6: log file should have ≥5 distinct warning entries (one per timeout)
    assert os.path.exists(_LOG_FILE), "log file should exist after concurrent invocations"
    with open(_LOG_FILE) as f:
        log_content = f.read()
    warning_count = log_content.count("[WARNING]")
    assert warning_count >= 5, \
        f"expected ≥5 WARNING entries in log (one per invocation), got {warning_count}.\n" \
        f"Full log:\n{log_content}"


# ═══════════════════════════════════════════════════════════════════
# T1.4: HTTP 200 with non-JSON model content falls back to {"raw": ...}
# ═══════════════════════════════════════════════════════════════════

def test_http_200_non_json_falls_back_to_raw(mock_non_json_server, capsys, monkeypatch):
    """T1.4: Mock server returns HTTP 200 with valid OpenAI JSON but model content = "not a json".

    The script's _extract_json should fall back to {"raw": "not a json"} (per docstring:
    "If the model's reply can't be parsed as JSON it is still returned under {"raw": "..."}").

    Note: The spec said "body 'not a json'" but the script's json.loads() on the HTTP
    response body would crash on non-JSON. The actual fallback path is in _extract_json
    which handles non-JSON model *content* (the OpenAI response is always JSON).
    This test correctly exercises that fallback.
    """
    monkeypatch.setenv("DASHSCOPE_API_KEY", "test-key-for-mock")
    _disable_proxies(monkeypatch)

    rc, stdout, stderr = _run_main_inprocess(mock_non_json_server, capsys)

    assert rc == 0, \
        f"expected return 0 for non-JSON content fallback, got {rc}. stderr: {stderr!r}"

    # stdout should be valid JSON with a "raw" key
    result = json.loads(stdout.strip())
    assert "raw" in result, \
        f"stdout JSON should contain 'raw' key, got: {result!r}"
    assert result["raw"] == "not a json", \
        f"'raw' value should be the model's content, got: {result['raw']!r}"


# ═══════════════════════════════════════════════════════════════════
# Bonus: HTTP 500 emits structured HTTPError log
# ═══════════════════════════════════════════════════════════════════

def test_http_500_emits_structured_error(mock_http_500_server, capsys, monkeypatch):
    """Bonus: HTTP 500 triggers HTTPError path with structured logging.

    Verifies the HTTPError handler logs correctly (one of the 4 structured log points).
    """
    monkeypatch.setenv("DASHSCOPE_API_KEY", "test-key-for-mock")
    _disable_proxies(monkeypatch)

    rc, stdout, stderr = _run_main_inprocess(mock_http_500_server, capsys)

    assert rc == 1, f"expected return 1 for HTTP 500, got {rc}"
    assert "HTTP 500" in stderr, \
        f"stderr should contain 'HTTP 500', got: {stderr!r}"

    # Log file should contain the HTTP warning
    assert os.path.exists(_LOG_FILE), "log file should exist"
    with open(_LOG_FILE) as f:
        log_content = f.read()
    recent = log_content[-500:]
    assert "HTTP 500" in recent or "HTTP" in recent, \
        f"log should mention HTTP error, last 500 chars: {recent!r}"
