"""Tests for Bridge request/response middleware (main.py)."""

from __future__ import annotations

import logging
import time

import pytest
from fastapi import FastAPI
from starlette.middleware.base import BaseHTTPMiddleware
from starlette.requests import Request
from starlette.responses import JSONResponse
from starlette.testclient import TestClient

from config import settings


# ── helpers ──────────────────────────────────────────────────────────────


def _make_app() -> FastAPI:
    """Build a minimal FastAPI app with the two middlewares and a test route."""
    from main import _RequestDebugMiddleware, _RequestTimingMiddleware

    app = FastAPI()
    app.add_middleware(_RequestDebugMiddleware)
    app.add_middleware(_RequestTimingMiddleware)

    @app.get("/api/test/ok")
    async def ok_route():
        return {"status": "ok"}

    @app.get("/api/test/slow")
    async def slow_route():
        time.sleep(1.5)
        return {"status": "slow"}

    @app.get("/api/test/error")
    async def error_route():
        from fastapi.responses import JSONResponse
        return JSONResponse({"error": "bad"}, status_code=400)

    return app


# ── _RequestTimingMiddleware tests ───────────────────────────────────────


class TestRequestTimingMiddleware:
    """TC-obs-01 ~ 04: 请求计时中间件"""

    def test_normal_request_not_logged(self, caplog: pytest.LogCaptureFixture):
        """正常请求 (elapsed<1s, status=200) 不应触发日志。"""
        caplog.set_level(logging.INFO)
        app = _make_app()
        with TestClient(app) as client:
            client.get("/api/test/ok")
        assert not any("http_request" in msg for msg in caplog.messages)

    def test_slow_request_logged(self, caplog: pytest.LogCaptureFixture):
        """慢请求 (elapsed>1s) 应记录 http_request 日志。"""
        caplog.set_level(logging.INFO)
        app = _make_app()
        with TestClient(app) as client:
            client.get("/api/test/slow")
        assert any(
            "http_request" in msg and "elapsed=1." in msg
            for msg in caplog.messages
        )

    def test_error_status_logged(self, caplog: pytest.LogCaptureFixture):
        """异常状态码 (>=400) 应记录 http_request 日志。"""
        caplog.set_level(logging.INFO)
        app = _make_app()
        with TestClient(app) as client:
            client.get("/api/test/error")
        assert any(
            "http_request" in msg and "status=400" in msg
            for msg in caplog.messages
        )

    def test_health_check_skipped(self, caplog: pytest.LogCaptureFixture):
        """健康检查 /api/health 不应触发日志。"""
        caplog.set_level(logging.INFO)
        app = _make_app()
        with TestClient(app) as client:
            client.get("/api/health")
        assert not any("http_request" in msg for msg in caplog.messages)


# ── _RequestDebugMiddleware tests ────────────────────────────────────────


class TestRequestDebugMiddleware:
    """TC-obs-05 ~ 07: DEBUG 请求/响应日志"""

    def test_debug_logs_request_body(self, caplog: pytest.LogCaptureFixture):
        """DEBUG 级别应打印请求 body。"""
        caplog.set_level(logging.DEBUG)
        app = _make_app()
        with TestClient(app) as client:
            client.post("/api/test/ok", json={"key": "value"})
        assert any(
            ">>>" in msg and "body=" in msg and "key" in msg
            for msg in caplog.messages
        )

    def test_debug_logs_response(self, caplog: pytest.LogCaptureFixture):
        """DEBUG 级别应打印响应 body。"""
        caplog.set_level(logging.DEBUG)
        app = _make_app()
        with TestClient(app) as client:
            client.get("/api/test/ok")
        assert any(
            "<<<" in msg and "status=200" in msg and "status" in msg
            for msg in caplog.messages
        )

    def test_info_level_skips_debug_logging(self, caplog: pytest.LogCaptureFixture):
        """INFO 级别不应打 >>>/<<< 日志。"""
        caplog.set_level(logging.INFO)
        app = _make_app()
        with TestClient(app) as client:
            client.get("/api/test/ok")
        assert not any(">>>" in msg or "<<<" in msg for msg in caplog.messages)
