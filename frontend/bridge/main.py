"""AgentFlow Bridge — FastAPI application entrypoint.

Provides:
    * CORS middleware (configured for the Vue dev server on :15173).
    * All business routes mounted under ``/health`` prefix on
      ``bridge_app``, with ``/api/health`` for gateway health checks.
    * ``GET /ws/chat/{agent_id}`` WebSocket endpoint for streaming agent
      conversations to the Vue frontend (mounted by ``routes.chat``).

Wave 2 tasks add the business routers under ``bridge.routes.*``; this
module is intentionally limited to the application skeleton.
"""

from __future__ import annotations

import asyncio
import logging
import time
from contextlib import asynccontextmanager
from typing import AsyncIterator

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from starlette.middleware.base import BaseHTTPMiddleware, RequestResponseEndpoint
from starlette.requests import Request
from starlette.responses import Response, StreamingResponse
from starlette.types import Message

from client import DaemonClient
from config import settings
from routes import agents as agents_routes
from routes import chat as chat_routes
from routes import daemon as daemon_routes
from routes import llm_trace as llm_trace_routes
from routes import sessions as sessions_routes
from routes.artifacts import router as artifacts_router
from routes.aisf_tasks import router as aisf_tasks_router
from routes.chat_rest import router as chat_rest_router
from routes.coremind_internal import router as coremind_internal_router
from routes.packages import router as packages_router
from routes.skill_hub import router as skill_hub_router
from routes.conversations_api import router as conversations_router

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

logger = logging.getLogger("agentflow.bridge")
logging.basicConfig(
    level=settings.log_level,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)

# Suppress noisy websocket keepalive DEBUG logs (module-level, runs early).
# Kept here as a baseline; lifespan hook re-suppresses after uvicorn overrides.
for _name in (
    "uvicorn.protocols.websockets",
    "uvicorn.protocols",
    "wsproto",
    "websockets",
    "websockets.client",
    "websockets.server",
):
    logging.getLogger(_name).setLevel(logging.INFO)


# ---------------------------------------------------------------------------
# Lifespan
# ---------------------------------------------------------------------------


@asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    """Application lifespan hook.

    Establishes the long-lived :class:`DaemonClient` attached to
    ``app.state.daemon_client`` for the lifetime of the process. The
    WebSocket chat router reads it from there so we have a single
    JSON-RPC connection shared by every concurrent chat.

    Also primes ``app.state.daemon_status_cache`` and starts a background
    poller that refreshes it every ``settings.daemon_status_refresh_interval``
    seconds, so the ``/api/daemon/*`` endpoints never block on the daemon.
    """
    # Re-suppress noisy websocket keepalive DEBUG logs after uvicorn has
    # overridden log levels (uvicorn --log-level debug overrides module-level
    # config, so we must re-apply here in the lifespan hook).
    for _name in (
        "uvicorn.protocols.websockets",
        "uvicorn.protocols",
        "wsproto",
        "websockets",
        "websockets.client",
        "websockets.server",
    ):
        logging.getLogger(_name).setLevel(logging.INFO)
    logger.info(
        "AgentFlow Bridge starting | bridge=%s:%s daemon=%s:%s packages=%s",
        settings.bridge_host,
        settings.bridge_port,
        settings.daemon_host,
        settings.daemon_port,
        settings.agent_packages_path,
    )

    # --- Daemon client -------------------------------------------------------
    daemon_client = DaemonClient()
    try:
        await daemon_client.connect()
        await asyncio.sleep(0.1)  # yield to reader task so it starts receiving
    except Exception as exc:  # noqa: BLE001
        # The bridge still boots: HTTP routes that need the daemon will
        # surface a clear error, and the chat WS endpoint rejects with a
        # 1011 close frame. We never want a daemon outage to wedge the
        # bridge HTTP server itself.
        logger.warning("DaemonClient initial connect failed: %s", exc)
    # Mounted sub-app lifespans aren't driven by the parent ASGI server.
    # Own the process lifecycle on the outer app, but attach shared runtime
    # state to bridge_app because routed requests/WebSockets see that app.
    runtime_state = bridge_app.state
    runtime_state.daemon_client = daemon_client
    app.state.daemon_client = daemon_client

    runtime_state.daemon_status_cache = dict(daemon_routes.EMPTY_CACHE)
    try:
        await daemon_routes.update_daemon_status_cache(runtime_state, daemon_client)
    except Exception as exc:  # noqa: BLE001
        logger.debug("initial daemon status prime failed (suppressed): %s", exc)

    poll_task = asyncio.create_task(
        _daemon_status_poll_loop(runtime_state, daemon_client),
        name="bridge.daemon_status_poll",
    )

    try:
        yield
    finally:
        poll_task.cancel()
        try:
            await poll_task
        except (asyncio.CancelledError, Exception):  # noqa: BLE001
            pass
        await daemon_client.close()
        logger.info("AgentFlow Bridge shutting down")


async def _daemon_status_poll_loop(state, client: DaemonClient) -> None:
    """Refresh the daemon status cache on a fixed interval forever.

    Sleeps in small chunks so shutdown is prompt: ``poll_task.cancel()``
    in the lifespan ``finally`` block will wake the loop within one
    chunk instead of waiting the full interval.

    If the client is not connected (e.g. initial connect failed before the
    daemon was ready), the loop transparently retries ``connect()`` so the
    bridge heals itself once the daemon comes online — no manual restart
    required.
    """
    interval = settings.daemon_status_refresh_interval
    idle = 0.5
    try:
        while True:
            slept = 0.0
            while slept < interval:
                chunk = min(idle, interval - slept)
                if chunk <= 0:
                    break
                await asyncio.sleep(chunk)
                slept += chunk

            # Transparent reconnection: if the client was never connected
            # (or lost its connection), retry connect() before refreshing
            # the status cache. This lets the bridge self-heal when the
            # daemon starts up after the bridge.
            if not client.is_connected:
                try:
                    await client.connect()
                except Exception as exc:  # noqa: BLE001
                    logger.debug("daemon reconnect attempt failed (suppressed): %s", exc)

            try:
                await daemon_routes.update_daemon_status_cache(state, client)
            except Exception as exc:  # noqa: BLE001
                logger.debug("daemon status refresh failed (suppressed): %s", exc)
    except asyncio.CancelledError:
        raise


# ---------------------------------------------------------------------------
# Application (internal — all business routes)
# ---------------------------------------------------------------------------

bridge_app = FastAPI(
    title="AgentFlow Bridge",
    version="0.1.0",
    description=(
        "HTTP + WebSocket bridge between the AgentFlow Vue frontend and "
        "the local agentflowd daemon. Proxies agent/session JSON-RPC and "
        "reads Agent Package files directly from the filesystem."
    ),
)

# --- CORS -------------------------------------------------------------------
# Allow the Vite dev server (and its 127.0.0.1 alias) to call the bridge
# during local development. Production deployments should tighten this list
# via the AGENTFLOW_CORS_ALLOWED_ORIGINS env var.
bridge_app.add_middleware(
    CORSMiddleware,
    allow_origins=settings.cors_allowed_origins,
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# --- 请求计时中间件 -----------------------------------------------------------
# 记录所有 HTTP 请求耗时和响应状态，排除 WebSocket 和健康检查的噪音
_SENSITIVE_HEADERS = {"authorization", "api-key", "x-api-key", "cookie"}
_MAX_BODY_LOG = 2048  # 最大请求体打印长度
_MAX_RESP_LOG = 4096  # 最大响应体打印长度

def _mask_headers(headers: dict) -> dict:
    """脱敏敏感 header 值，仅保留前 8 字符。"""
    masked = {}
    for k, v in headers.items():
        if k.lower() in _SENSITIVE_HEADERS and v:
            masked[k] = v[:8] + "..."
        else:
            masked[k] = v
    return masked

class _RequestDebugMiddleware(BaseHTTPMiddleware):
    """DEBUG 级别打印请求/响应详细内容（body + headers）。

    仅在 root logger 的 level ≤ DEBUG 时生效，避免生产环境性能开销。
    响应体通过包装 StreamingResponse 捕获，不影响下游流式传输。
    """

    async def dispatch(self, request: Request, call_next: RequestResponseEndpoint) -> Response:
        if not logger.isEnabledFor(logging.DEBUG):
            return await call_next(request)

        if request.url.path.startswith("/ws/"):
            return await call_next(request)

        # ── 读取请求体（需 buffer 后重新注入） ──
        body_bytes = await request.body()
        body_preview = body_bytes[:_MAX_BODY_LOG].decode("utf-8", errors="replace")
        if len(body_bytes) > _MAX_BODY_LOG:
            body_preview += f" … (truncated, {len(body_bytes)} bytes total)"

        logger.debug(
            ">>> %s %s headers=%s body=%s",
            request.method, request.url.path,
            _mask_headers(dict(request.headers)),
            body_preview,
        )

        # ── 包装响应以捕获 body ──
        response = await call_next(request)

        if isinstance(response, StreamingResponse):
            # 对于流式响应，收集 body chunks 后重组
            chunks: list[bytes] = []
            async for chunk in response.body_iterator:
                chunks.append(chunk)
            resp_body = b"".join(chunks)
            resp_preview = resp_body[:_MAX_RESP_LOG].decode("utf-8", errors="replace")
            if len(resp_body) > _MAX_RESP_LOG:
                resp_preview += f" … (truncated, {len(resp_body)} bytes total)"
            logger.debug(
                "<<< %s %s status=%d body=%s",
                request.method, request.url.path, response.status_code, resp_preview,
            )
            # 重新构造 StreamingResponse 让下游消费
            return StreamingResponse(
                iter([resp_body]),
                status_code=response.status_code,
                headers=dict(response.headers),
                media_type=response.media_type,
            )

        # 非流式响应直接读取
        resp_body = b""
        if hasattr(response, "body"):
            resp_body = response.body or b""
        resp_preview = resp_body[:_MAX_RESP_LOG].decode("utf-8", errors="replace")
        if len(resp_body) > _MAX_RESP_LOG:
            resp_preview += f" … (truncated, {len(resp_body)} bytes total)"
        logger.debug(
            "<<< %s %s status=%d body=%s",
            request.method, request.url.path, response.status_code, resp_preview,
        )
        return response

# --- 请求计时中间件 -----------------------------------------------------------
class _RequestTimingMiddleware(BaseHTTPMiddleware):
    async def dispatch(self, request: Request, call_next) -> Response:
        if request.url.path in ("/api/health",) or request.url.path.startswith("/ws/"):
            return await call_next(request)
        start = time.time()
        response = await call_next(request)
        elapsed = time.time() - start
        if elapsed > 1.0 or response.status_code >= 400:
            logger.info(
                "http_request %s %s status=%d elapsed=%.2fs",
                request.method, request.url.path, response.status_code, elapsed,
            )
        return response

# 注册顺序：debug 在外层（先经过），timing 在内层
bridge_app.add_middleware(_RequestDebugMiddleware)
bridge_app.add_middleware(_RequestTimingMiddleware)
bridge_app.include_router(artifacts_router)
bridge_app.include_router(coremind_internal_router)


# ---------------------------------------------------------------------------
# Routes (on bridge_app)
# ---------------------------------------------------------------------------


# WebSocket chat router reads the shared DaemonClient from
# ``app.state.daemon_client``, which is set by the lifespan hook above.
bridge_app.include_router(chat_routes.router)


# REST chat router — synchronous request/response alternative to the
# WebSocket streaming endpoint above.
bridge_app.include_router(chat_rest_router)


# Skill Hub router — skill central repository CRUD + install.
bridge_app.include_router(skill_hub_router)


# --- API routers -------------------------------------------------------------
# The agent and session routers are pure HTTP-to-JSON-RPC proxies. They are
# registered with their own ``prefix`` so the full mount path is already
# ``/api/agents`` and ``/api/sessions`` — do NOT wrap with another ``/api``.
bridge_app.include_router(agents_routes.router)
bridge_app.include_router(sessions_routes.router)


# Mount the agent package router. T5 (CRUD) and T8 (packaging) live in
# the same module so they share the ``/api/packages`` prefix and the
# read-only surface can be extended in one place.
bridge_app.include_router(packages_router)


# Daemon monitoring routes. The router ships with its own ``/api/daemon``
# prefix, so the full mount path is ``/api/daemon/status`` and
# ``/api/daemon/uptime`` — no extra wrapping needed.
bridge_app.include_router(daemon_routes.router)


# LLM Trace routes — Phase 1, reads DataBus UDS directly (bypasses daemon).
# These are mounted with their own ``/api/agents/…`` prefix inside the module.
bridge_app.include_router(llm_trace_routes.router)


# Conversation history — persists completed turn responses keyed by
# ``(user_id, client_id)`` so the frontend can list past conversations
# independently of the daemon's ephemeral dialogue lifecycle.
bridge_app.include_router(conversations_router)


# Health check (accessible at /health/api/health)
@bridge_app.get("/api/health", tags=["meta"], include_in_schema=False)
async def bridge_health() -> dict[str, str]:
    return {"status": "ok", "service": "agentflow-bridge"}


# ---------------------------------------------------------------------------
# Application (external — entrypoint with prefix mount)
# ---------------------------------------------------------------------------
# The outer ``app`` exposes only health probes at the root; all business
# routes live on ``bridge_app`` mounted at ``/health``.

app = FastAPI(title="AgentFlow Bridge (external)", lifespan=lifespan)


# Health check callers go through the gateway which adds the
# /health prefix, so only bridge_app serves /api/health.
# The outer app has no business routes — everything is mounted below.

##最新修改
app.mount("/health", bridge_app)

# AISF SysAgent 调度入口（独立路由，不经过 /health 挂载前缀）
app.include_router(aisf_tasks_router)


# ---------------------------------------------------------------------------
# Entrypoint
# ---------------------------------------------------------------------------


def run() -> None:
    """Console-script entrypoint (``agentflow-bridge``)."""
    import uvicorn

    uvicorn.run(
        "main:app",
        host=settings.bridge_host,
        port=settings.bridge_port,
        log_level=settings.log_level.lower(),
        reload=False,
        max_request_body_size=settings.upload_max_body_size_mb * 1024 * 1024,
    )


if __name__ == "__main__":
    run()
