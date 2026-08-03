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
import logging.handlers
import subprocess
import sys
import time
from contextlib import asynccontextmanager
from pathlib import Path
from typing import AsyncIterator

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from starlette.middleware.base import BaseHTTPMiddleware, RequestResponseEndpoint
from starlette.requests import Request
from starlette.responses import Response, StreamingResponse
from starlette.types import Message

from client import DaemonClient, DaemonConnectionError, DaemonTimeoutError
from config import settings
from providers import LocalSessionProvider
from services import AgentflowSessionSync
from services.sandbox_observer import SandboxObserver
from routes import agents as agents_routes
from routes import chat as chat_routes
from routes import daemon as daemon_routes
from routes import llm_trace as llm_trace_routes
from routes import sandbox as sandbox_routes
from routes import sessions as sessions_routes
from routes.artifacts import router as artifacts_router
from routes.aisf_tasks import router as aisf_tasks_router
from routes.chat_rest import router as chat_rest_router
from routes.packages import router as packages_router
from routes.sandbox_files import router as sandbox_files_router
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

# FileHandler 兜底：手动启动 uvicorn 时 stdout 未重定向，日志会丢失到 /private/tmp/。
_PROJECT_ROOT = Path(__file__).resolve().parents[2]
_LOG_DIR = _PROJECT_ROOT / "logs"
_LOG_DIR.mkdir(exist_ok=True)
_LOG_FILE = _LOG_DIR / "bridge.log"

_root_logger = logging.getLogger()
if not any(isinstance(h, logging.FileHandler) for h in _root_logger.handlers):
    _fh = logging.handlers.RotatingFileHandler(
        str(_LOG_FILE), maxBytes=10 * 1024 * 1024, backupCount=5, encoding="utf-8",
    )
    _fh.setFormatter(logging.Formatter(
        "%(asctime)s [%(levelname)s] %(name)s: %(message)s"))
    _root_logger.addHandler(_fh)
    # 移除 basicConfig 添加的 StreamHandler（stderr）——start_bridge.sh 已把
    # stdout/stderr 重定向进 bridge.log（>> file 2>&1），StreamHandler 与 FileHandler
    # 会写同一文件导致同一行日志打印双份。FileHandler 是唯一文件落点。
    for _h in list(_root_logger.handlers):
        if isinstance(_h, logging.StreamHandler) and not isinstance(_h, logging.FileHandler):
            _root_logger.removeHandler(_h)
            logger.info("Removed duplicate StreamHandler (stderr redirected to %s)", _LOG_FILE)
    logger.info("FileHandler attached: %s", _LOG_FILE)

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

    # --- Session persistence provider ---------------------------------------
    # LocalSessionProvider is the Phase-1 default; Phase-3 will swap to
    # KMMSessionProvider based on ``settings.session_provider``.
    if settings.session_provider == "local":
        session_provider = LocalSessionProvider(settings.session_db_path_resolved)
    else:
        # Phase 3: KMM provider — fall back to Local for now so the bridge
        # still boots on misconfigured deployments.
        logger.warning(
            "session_provider=%s not yet implemented, falling back to local",
            settings.session_provider,
        )
        session_provider = LocalSessionProvider(settings.session_db_path_resolved)
    runtime_state.session_provider = session_provider
    app.state.session_provider = session_provider

    runtime_state.daemon_status_cache = dict(daemon_routes.EMPTY_CACHE)
    try:
        await daemon_routes.update_daemon_status_cache(runtime_state, daemon_client)
    except Exception as exc:  # noqa: BLE001
        logger.debug("initial daemon status prime failed (suppressed): %s", exc)

    poll_task = asyncio.create_task(
        _daemon_status_poll_loop(runtime_state, daemon_client),
        name="bridge.daemon_status_poll",
    )

    # --- Sandbox observer (local dev observability) ---------------------------
    # 观测适配层按 sandbox.mode 分派数据源（e2b SDK / daemon 日志 / 目录扫描）；
    # local/docker 模式的 kill/prune 经 daemon_client 调 sandbox.destroy RPC（E2），
    # daemon 离线时构造不失败（_daemon_client=None，kill/prune 返回 502 兜底）。
    # 初始化失败不阻断 Bridge 启动（路由返回 502 兜底）。
    try:
        runtime_state.sandbox_observer = SandboxObserver(settings, daemon_client=daemon_client)
        app.state.sandbox_observer = runtime_state.sandbox_observer
        logger.info("sandbox observer initialized mode=%s", settings.sandbox_mode)
    except Exception as exc:  # noqa: BLE001
        logger.warning("sandbox observer init failed (suppressed): %s", exc)

    # --- Session status sync (Phase 2) --------------------------------------
    # Best-effort background sync of agentflow→Bridge session status.
    # Interval ≤ 0 disables the poller (sync still runs once at start).
    session_sync = AgentflowSessionSync(daemon_client, session_provider)
    try:
        await session_sync.sync()
    except Exception as exc:  # noqa: BLE001
        logger.debug("initial session sync failed (suppressed): %s", exc)
    sync_task: asyncio.Task | None = None
    if settings.session_sync_interval > 0:
        sync_task = asyncio.create_task(
            _session_sync_loop(session_sync, daemon_client, settings.session_sync_interval),
            name="bridge.session_sync_poll",
        )

    # --- §2.2 预装 agentflow-runtime 到系统 python3 ---------------------------
    # shell wrapper fallback 时依赖系统 python3 已预装 agentflow-runtime。
    # 失败不阻断 Bridge 启动（只 WARN）——预装缺失只影响 fallback 分支，
    # .venv 就绪的 Agent 仍可正常运行。
    try:
        subprocess.run(
            [sys.executable, "-m", "pip", "install", "--quiet", "agentflow-runtime"],
            check=False,
            capture_output=True,
            timeout=120,
        )
        logger.info("preload agentflow-runtime to system python (%s)", sys.executable)
    except Exception as exc:  # noqa: BLE001
        logger.warning(
            "preload agentflow-runtime failed (shell wrapper fallback may break): %s",
            exc,
        )

    try:
        yield
    finally:
        poll_task.cancel()
        if sync_task is not None:
            sync_task.cancel()
        try:
            await poll_task
        except (asyncio.CancelledError, Exception):  # noqa: BLE001
            pass
        if sync_task is not None:
            try:
                await sync_task
            except (asyncio.CancelledError, Exception):  # noqa: BLE101
                pass
        await daemon_client.close()
        logger.info("AgentFlow Bridge shutting down")


async def _session_sync_loop(
    syncer: AgentflowSessionSync, daemon_client: DaemonClient, interval: float,
) -> None:
    """Background poll loop driving ``AgentflowSessionSync.sync()``.

    Sleeps in small chunks so shutdown is prompt (mirrors
    ``_daemon_status_poll_loop``). All sync errors are swallowed inside
    ``sync()`` so the loop never dies on a daemon/provider outage.

    When the daemon connection times out (e.g. WebSocket silently dropped),
    the client is closed and reconnected so the next cycle doesn't hang.
    """
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
            try:
                await syncer.sync()
            except asyncio.CancelledError:
                raise
            except (DaemonTimeoutError, DaemonConnectionError) as exc:
                logger.warning(
                    "session_sync.reconnect daemon error: %s", exc,
                )
                try:
                    await daemon_client.close()
                except Exception:
                    pass
                try:
                    await daemon_client.connect(max_retries=2, retry_delay=1.0)
                    logger.info("session_sync.reconnect ok")
                except Exception as conn_exc:
                    logger.warning(
                        "session_sync.reconnect_failed: %s", conn_exc,
                    )
            except Exception as exc:  # noqa: BLE001
                logger.debug("session sync loop error (suppressed): %s", exc)
    except asyncio.CancelledError:
        raise


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
    """INFO 级别打印请求/响应详细内容（body + headers）。

    仅在 root logger 的 level ≤ INFO 时生效；WARNING 及以上级别关闭，
    避免生产环境性能开销。响应体通过包装 StreamingResponse 捕获，
    不影响下游流式传输。
    """

    async def dispatch(self, request: Request, call_next: RequestResponseEndpoint) -> Response:
        if not logger.isEnabledFor(logging.INFO):
            return await call_next(request)

        if request.url.path.startswith("/ws/"):
            return await call_next(request)

        # ── 读取请求体（需 buffer 后重新注入） ──
        body_bytes = await request.body()
        body_preview = body_bytes[:_MAX_BODY_LOG].decode("utf-8", errors="replace")
        if len(body_bytes) > _MAX_BODY_LOG:
            body_preview += f" … (truncated, {len(body_bytes)} bytes total)"

        logger.info(
            ">>> %s %s headers=%s body=%s",
            request.method, request.url.path,
            _mask_headers(dict(request.headers)),
            body_preview,
        )

        # ── 包装响应以捕获 body ──
        response = await call_next(request)

        # SSE 流式响应（text/event-stream）：收集全部 chunks 再重组会破坏流式
        # 实时性（前端要等整个流结束才能收到），直接透传不捕获 body。
        if response.headers.get("content-type", "").startswith("text/event-stream"):
            logger.info(
                "<<< %s %s status=%d body=<streaming>",
                request.method, request.url.path, response.status_code,
            )
            return response

        # Starlette BaseHTTPMiddleware 会把下游响应包装为内部 _StreamingResponse
        # （继承 Response 而非 StreamingResponse，仅有 body_iterator），
        # 因此不能只用 isinstance(StreamingResponse) 判断——统一按 body_iterator 处理。
        if hasattr(response, "body_iterator"):
            # 收集 body chunks 后重组（跳过 ASGI 控制消息，如 pathsend）
            chunks: list[bytes] = []
            async for chunk in response.body_iterator:
                if isinstance(chunk, bytes):
                    chunks.append(chunk)
            resp_body = b"".join(chunks)
            resp_preview = resp_body[:_MAX_RESP_LOG].decode("utf-8", errors="replace")
            if len(resp_body) > _MAX_RESP_LOG:
                resp_preview += f" … (truncated, {len(resp_body)} bytes total)"
            logger.info(
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
        logger.info(
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


# Sandbox management routes (local dev observability). Own ``/api/sandbox``
# prefix; reads SandboxObserver from ``app.state.sandbox_observer``.
bridge_app.include_router(sandbox_routes.router)


# LLM Trace routes — Phase 1, reads DataBus UDS directly (bypasses daemon).
# These are mounted with their own ``/api/agents/…`` prefix inside the module.
bridge_app.include_router(llm_trace_routes.router)


# Conversation history — persists completed turn responses keyed by
# ``(user_id, client_id)`` so the frontend can list past conversations
# independently of the daemon's ephemeral dialogue lifecycle.
bridge_app.include_router(conversations_router)
bridge_app.include_router(sandbox_files_router)


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

# Artifact 文件服务（外层 app 直挂，本地 dev 通过 Vite proxy /api 可达；
# 生产环境通过网关加 /health 前缀 → bridge_app 下的 artifacts 路由同样可处理）
app.include_router(artifacts_router)


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
