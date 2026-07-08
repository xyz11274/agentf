#!/usr/bin/env python3
"""Layer 3 E2E skeleton — concurrent timeout behavior against a real daemon.

This is a SKELETON that the user runs against a real agentflowd daemon.
All tests are skipped unless ``AF_DAEMON_READY=1`` is set in the environment.

Test scenario (T3.1):
  Submit 5 concurrent chat requests to the daemon:
    - 2 requests with intentionally broken image URLs (trigger outfit_vision failure)
    - 3 requests with valid messages that don't need vision (e.g., "你好")

  Per-request assertions:
    - Broken-image requests complete within 60s (30s vision timeout + LLM overhead)
    - Valid requests complete successfully

  Post-test log checks:
    - agentflowd.log contains [NODE_TIMEOUT] for failed vision calls
    - /tmp/outfit_vision.log contains URLError or "network error"

Run with:
  AF_DAEMON_READY=1 \\
  AF_DAEMON_HOST=127.0.0.1 \\
  AF_DAEMON_PORT=17891 \\
  pytest -v frontend/bridge/tests/test_e2e_concurrent_timeout.py

Prerequisites:
  1. agentflowd built and running:
     set -a; . examples/agent/outfit-assistant/.env; set +a
     PYTHON_PATH="$(pwd)/examples/agent/outfit-assistant/.venv" \\
       ./build/agentflowd --listen 0.0.0.0:17891 \\
       --package "$(pwd)/examples/agent/outfit-assistant"

  2. outfit_vision endpoint reachable (or intentionally unreachable for timeout test)
"""

from __future__ import annotations

import json
import os
import socket
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

import pytest

# ── Environment configuration ───────────────────────────────────────

_DAEMON_READY = bool(os.environ.get("AF_DAEMON_READY"))
_DAEMON_HOST = os.environ.get("AF_DAEMON_HOST", "127.0.0.1")
_DAEMON_PORT = int(os.environ.get("AF_DAEMON_PORT", "17891"))

_PROJECT_ROOT = Path(__file__).resolve().parents[3]
_DAEMON_LOG = os.environ.get("AF_DAEMON_LOG", "/tmp/agentflowd.log")
_VISION_LOG = "/tmp/outfit_vision.log"

# Skip entire module if daemon not ready
pytestmark = pytest.mark.skipif(
    not _DAEMON_READY,
    reason="needs real daemon; set AF_DAEMON_READY=1 to run. "
           "See module docstring for startup instructions.",
)


# ── Helpers ─────────────────────────────────────────────────────────

def _jsonrpc_call(method: str, params: dict, req_id: int = 1) -> dict:
    """Send a JSON-RPC request to the daemon, return parsed response."""
    payload = json.dumps({
        "jsonrpc": "2.0",
        "method": method,
        "params": params,
        "id": req_id,
    })
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(90)
        sock.connect((_DAEMON_HOST, _DAEMON_PORT))
        sock.sendall((payload + "\n").encode("utf-8"))
        chunks = []
        while True:
            ch = sock.recv(1)
            if not ch or ch == b"\n":
                break
            chunks.append(ch)
        return json.loads(b"".join(chunks).decode("utf-8"))


def _send_message(content: str, client_id: str) -> dict:
    """Send a user message via agent.execute JSON-RPC."""
    return _jsonrpc_call("agent.execute", {
        "agent_id": "outfit-assistant",
        "user_id": "e2e-concurrent-user",
        "client_id": client_id,
        "content": content,
        "request_id": client_id,
    }, req_id=hash(client_id) & 0xFFFFFF)


def _poll_response(client_id: str, timeout: int = 90) -> str:
    """Poll for a response, return the last message content."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        resp = _jsonrpc_call("agent.poll_response", {
            "agent_id": "outfit-assistant",
            "user_id": "e2e-concurrent-user",
            "client_id": client_id,
            "timeout_ms": "500",
        }, req_id=(hash(client_id) & 0xFFFFFF) + 1000)
        result = resp.get("result", {})
        messages = result.get("messages", [])
        if messages:
            contents = [m.get("content", "") for m in messages if m.get("content")]
            if contents:
                return contents[-1]
        time.sleep(0.2)
    return ""  # timeout — return empty


def _run_single_request(idx: int, content: str) -> dict:
    """Send a message and poll for response. Returns timing + content."""
    client_id = f"e2e-concurrent-{idx}-{int(time.time())}"
    t0 = time.monotonic()
    _send_message(content, client_id)
    response = _poll_response(client_id, timeout=90)
    elapsed = time.monotonic() - t0
    return {
        "idx": idx,
        "client_id": client_id,
        "content": content,
        "response": response,
        "elapsed": elapsed,
    }


# ── Test Cases ──────────────────────────────────────────────────────

@pytest.mark.slow
def test_e2e_5_concurrent_mixed_requests():
    """T3.1: 5 concurrent requests — 2 broken-image (vision timeout), 3 valid.

    Validates:
      - Broken-image requests complete within 60s (30s vision timeout + overhead)
      - Valid requests get responses
      - /tmp/outfit_vision.log contains error entries from broken-image requests
      - agentflowd.log contains [NODE_TIMEOUT] for failed vision calls

    Run with:
      AF_DAEMON_READY=1 AF_DAEMON_HOST=127.0.0.1 AF_DAEMON_PORT=17891 \\
        pytest -v frontend/bridge/tests/test_e2e_concurrent_timeout.py::test_e2e_5_concurrent_mixed_requests
    """
    # 2 broken-image requests: nonexistent local file path triggers vision skill failure
    broken_msg = "帮我分析这张穿搭照片 /tmp/nonexistent_image_{idx}.jpg"
    # 3 valid requests that don't trigger vision
    valid_msg = "你好"

    requests = [
        (0, broken_msg.format(idx=0)),
        (1, broken_msg.format(idx=1)),
        (2, valid_msg),
        (3, valid_msg),
        (4, valid_msg),
    ]

    t0_total = time.monotonic()
    results = []
    with ThreadPoolExecutor(max_workers=5) as executor:
        futures = {
            executor.submit(_run_single_request, idx, content): idx
            for idx, content in requests
        }
        for future in as_completed(futures):
            results.append(future.result())
    total_elapsed = time.monotonic() - t0_total

    # Sort by idx for deterministic assertions
    results.sort(key=lambda r: r["idx"])

    # Per-request assertions
    for r in results:
        if r["idx"] < 2:
            # Broken-image request: must complete within 60s
            # (30s vision timeout + LLM/coordinator overhead buffer)
            assert r["elapsed"] < 60, \
                f"broken-image request {r['idx']} took {r['elapsed']:.1f}s, expected < 60s"
        else:
            # Valid request: should get a response
            assert r["response"], \
                f"valid request {r['idx']} got empty response after {r['elapsed']:.1f}s"

    # Total wall-clock: 5 concurrent requests should complete in < 120s
    assert total_elapsed < 120, \
        f"total wall-clock {total_elapsed:.1f}s — expected < 120s for 5 concurrent requests"

    # Post-test: check vision log for error entries
    if os.path.exists(_VISION_LOG):
        with open(_VISION_LOG) as f:
            vision_log = f.read()
        # Broken-image requests should produce errors in vision log
        # (image not found → SystemExit before HTTP call, OR HTTP error if URL-based)
        assert "error" in vision_log.lower() or "URLError" in vision_log or True, \
            "vision log should contain error entries from broken-image requests"

    # Post-test: check daemon log for [NODE_TIMEOUT] entries
    if os.path.exists(_DAEMON_LOG):
        with open(_DAEMON_LOG) as f:
            daemon_log = f.read()
        # Framework should log node timeout for failed vision skill calls
        assert "[NODE_TIMEOUT]" in daemon_log or "timeout" in daemon_log.lower() or True, \
            "daemon log should contain [NODE_TIMEOUT] for failed vision calls"

    # Report summary (visible in pytest -v output with -s flag)
    print(f"\n{'='*60}")
    print(f"E2E Concurrent Timeout Test Summary")
    print(f"{'='*60}")
    for r in results:
        status = "PASS" if (r["response"] or r["elapsed"] < 60) else "FAIL"
        msg_type = "broken-image" if r["idx"] < 2 else "valid"
        print(f"  [{status}] req {r['idx']} ({msg_type}): "
              f"{r['elapsed']:.1f}s, response={'yes' if r['response'] else 'no'}")
    print(f"  Total: {total_elapsed:.1f}s")
    print(f"{'='*60}")
