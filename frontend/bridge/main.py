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
from starlette.middleware.base import BaseHTTPMiddleware
from starlette.requests import Request
from starlette.responses import Response

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

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

logger = logging.getLogger("agentflow.bridge")
logging.basicConfig(
    level=settings.log_level,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)


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

bridge_app.add_middleware(_RequestTimingMiddleware)
bridge_app.include_router(artifacts_router)
bridge_app.include_router(coremind_internal_router)
# AISF SysAgent 调度入口：POST /v1/tasks → 对外 /health/v1/tasks
bridge_app.include_router(aisf_tasks_router)


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
