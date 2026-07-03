"""AgentFlow Bridge — daemon JSON-RPC client over WebSocket.

Transport: WebSocket (RFC 6455), JSON-RPC 2.0.

This replaced the old raw-TCP transport because the daemon's TCP port
handles both raw TCP and WebSocket connections.  The daemon now correctly
wraps ``send_notification()`` push messages in WebSocket frames, so the
original reason for using raw TCP (Gap 09) no longer applies.

The wire format is:
  Request:  ``{"jsonrpc":"2.0","method":"...","params":{...},"id":N}``
  Response: ``{"jsonrpc":"2.0","result":...,"id":N}``
  Notif:    ``{"jsonrpc":"2.0","method":"agent.response",...}``

All messages travel as WebSocket text frames.  No the ``\\n`` delimiter or
manual frame reassembly is needed.
"""

from __future__ import annotations

import asyncio
import itertools
import json
import logging
from typing import Any, Awaitable, Callable, Optional

import websockets

from config import BridgeSettings, settings as _default_settings

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Exceptions
# ---------------------------------------------------------------------------


def _looks_like_complete_json(text: str) -> bool:
    """Is ``text`` a complete JSON value?

    Uses ``json.JSONDecoder.raw_decode`` as the authoritative check.
    The old brace-balance heuristic caused false positives when a TCP
    fragment happened to have balanced braces but was missing trailing fields;
    the orphaned tail (e.g. ``',"id":52}'``) then failed to parse
    (see ``test_truncated_tail_from_logs`` for the exact shape).

    Retained for backward compatibility with existing tests; no longer used
    internally by ``DaemonClient`` (WebSocket provides complete frames).
    """
    t = text.strip()
    if not t:
        return True
    if t[0] not in '{["':
        return False
    try:
        decoder = json.JSONDecoder()
        _, end = decoder.raw_decode(t, 0)  # type: ignore[arg-type]
        return end == len(t)
    except (TypeError, ValueError):
        return False


class DaemonError(Exception):
    """Base class for every daemon-side error surfaced by ``DaemonClient``."""

    def __init__(self, message: str, *, code: Optional[int] = None) -> None:
        super().__init__(message)
        self.code = code
        self.message = message

    def __str__(self) -> str:
        return self.message


class DaemonConnectionError(DaemonError):
    """Raised when the underlying WebSocket connection cannot be opened."""


class DaemonTimeoutError(DaemonError):
    """Raised when a request does not receive a response within ``timeout``."""


class InvalidParamsError(DaemonError):
    """JSON-RPC ``-32602`` — invalid method parameters."""


class NotFoundError(DaemonError):
    """Daemon code ``-2`` — referenced resource not found."""


_ERROR_MAP: dict[int, type[DaemonError]] = {
    -32602: InvalidParamsError,
    -2: NotFoundError,
    -1: DaemonError,
}


# ---------------------------------------------------------------------------
# Client
# ---------------------------------------------------------------------------


class DaemonClient:
    """WebSocket JSON-RPC 2.0 client for the agentflowd daemon.

    Uses a single persistent WebSocket connection for all requests and
    receives push notifications (``agent.response``, etc.) via the same
    connection.  The background reader task handles incoming frames and
    dispatches them to pending futures or the notification queue.
    """

    def __init__(
        self,
        host: Optional[str] = None,
        port: Optional[int] = None,
        *,
        ws_url: Optional[str] = None,
        timeout: Optional[float] = None,
        bridge_settings: Optional[BridgeSettings] = None,
    ) -> None:
        self._bridge_settings = bridge_settings or _default_settings
        if ws_url:
            self._ws_url = ws_url
        else:
            h = host or self._bridge_settings.daemon_host
            p = port or self._bridge_settings.daemon_port
            self._ws_url = f"ws://{h}:{p}"
        self._timeout = (
            timeout if timeout is not None else self._bridge_settings.request_timeout_seconds
        )

        self._id_counter = itertools.count(1)
        self._pending: dict[int, asyncio.Future[dict[str, Any]]] = {}
        self._notification_queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue()
        self._response_queues: dict[str, asyncio.Queue[dict[str, Any]]] = {}
        self._ws: Optional[websockets.WebSocketClientProtocol] = None
        self._connected = False
        self._closing = False
        self._reader_task: Optional[asyncio.Task[None]] = None

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------

    async def connect(self) -> None:
        """Open a WebSocket connection and start the reader task."""
        if self._connected:
            return
        if self._reader_task is not None and not self._reader_task.done():
            return

        try:
            self._ws = await asyncio.wait_for(
                websockets.connect(
                    self._ws_url,
                    ping_interval=20,
                    ping_timeout=10,
                    max_size=2 ** 20,  # 1 MB frame limit
                ),
                timeout=self._timeout,
            )
        except Exception as exc:
            self._connected = False
            raise DaemonConnectionError(
                f"failed to connect to {self._ws_url}: {exc}"
            ) from exc

        self._connected = True
        self._closing = False
        self._reader_task = asyncio.create_task(
            self._ws_reader(), name="DaemonClient.ws-reader"
        )
        logger.info("daemon_connect.connected url=%s", self._ws_url)

    async def close(self) -> None:
        """Gracefully shut down the client."""
        if self._closing and not self._connected:
            return
        self._closing = True

        if self._reader_task is not None and not self._reader_task.done():
            self._reader_task.cancel()
        if self._reader_task is not None:
            try:
                await self._reader_task
            except (asyncio.CancelledError, Exception):
                pass

        self._fail_all_pending(DaemonConnectionError("client closed"))

        if self._ws is not None:
            try:
                await self._ws.close()
            except Exception:
                logger.debug("daemon_connect.close_error", exc_info=True)

        self._ws = None
        self._connected = False
        logger.info("daemon_connect.closed url=%s", self._ws_url)
        logger.info("DaemonClient closed")

    @property
    def is_connected(self) -> bool:
        return self._connected

    # ------------------------------------------------------------------
    # Core request/response
    # ------------------------------------------------------------------

    async def send_request(
        self,
        method: str,
        params: Optional[dict[str, Any]] = None,
        timeout: Optional[float] = None,
    ) -> Any:
        """Send a JSON-RPC 2.0 request and return the daemon's ``result`` payload.

        Serialises the request as a WebSocket text frame.
        """
        if not self._connected or self._ws is None:
            raise DaemonConnectionError("client is not connected")

        request_id = next(self._id_counter)
        loop = asyncio.get_running_loop()
        future: asyncio.Future[dict[str, Any]] = loop.create_future()
        self._pending[request_id] = future

        frame = self._encode_request(request_id, method, params or {})
        try:
            await self._ws.send(frame)
        except Exception as exc:
            self._pending.pop(request_id, None)
            raise DaemonConnectionError(
                f"failed to send request {method!r}: {exc}"
            ) from exc

        effective_timeout = timeout if timeout is not None else self._timeout
        try:
            response = await asyncio.wait_for(future, timeout=effective_timeout)
        except asyncio.TimeoutError as exc:
            self._pending.pop(request_id, None)
            raise DaemonTimeoutError(
                f"timeout waiting for response to {method!r} after "
                f"{effective_timeout}s",
                code=-32000,
            ) from exc
        finally:
            self._pending.pop(request_id, None)

        if "error" in response:
            raise self._build_daemon_error(response["error"], method)
        return response.get("result")

    # ------------------------------------------------------------------
    # Convenience wrappers
    # ------------------------------------------------------------------

    async def health(self) -> dict[str, Any]:
        return await self.send_request("health", {})

    async def agent_list(self) -> list[dict[str, Any]]:
        return await self.send_request("agent.list", {})

    async def agent_info(self, agent_id: str) -> dict[str, Any]:
        return await self.send_request("agent.info", {"agent_id": agent_id})

    async def agent_load(self, package_path: str) -> dict[str, Any]:
        return await self.send_request("agent.load", {"package_path": package_path})

    async def agent_unload(self, agent_id: str) -> dict[str, Any]:
        return await self.send_request("agent.unload", {"agent_id": agent_id})

    async def agent_pause(self, agent_id: str) -> dict[str, Any]:
        return await self.send_request("agent.pause", {"agent_id": agent_id})

    async def agent_resume(self, agent_id: str) -> dict[str, Any]:
        return await self.send_request("agent.resume", {"agent_id": agent_id})

    async def agent_execute(
        self,
        agent_id: str,
        session_id: str,
        content: str,
        user_id: str = "",
        client_id: str = "bridge",
        dialogue_id: str = "",
        platform_context: dict[str, Any] | None = None,
        request_id: str = "",
    ) -> dict[str, Any]:
        params: dict[str, Any] = {
            "agent_id": agent_id,
            "user_id": user_id or "bridge-user",
            "client_id": client_id,
            "content": content,
        }
        if dialogue_id:
            params["dialogue_id"] = dialogue_id
            # Register before sending agent.execute: a fast daemon may push the
            # final notification immediately after the RPC response.
            self._response_queues.setdefault(dialogue_id, asyncio.Queue())
        elif session_id:
            params["dialogue_id"] = session_id
            self._response_queues.setdefault(session_id, asyncio.Queue())
        if platform_context is not None:
            params["platform_context"] = platform_context
        if request_id:
            params["request_id"] = request_id
        try:
            return await self.send_request("agent.execute", params)
        except Exception:
            response_key = dialogue_id or session_id
            if response_key:
                self._response_queues.pop(response_key, None)
            raise

    async def agent_poll_response(
        self,
        agent_id: str,
        session_id: str,
        task_id: str,
        timeout_ms: int = 15000,
    ) -> dict[str, Any]:
        return await self.send_request(
            "agent.poll_response",
            {
                "agent_id": agent_id,
                "session_id": session_id,
                "task_id": task_id,
                "timeout_ms": timeout_ms,
            },
        )

    async def wait_for_response(
        self,
        agent_id: str,
        *,
        dialogue_id: str = "",
        timeout: float = 30.0,
    ) -> dict[str, Any]:
        """Wait for an ``agent.response`` notification pushed by the daemon.

        The daemon pushes ``{"jsonrpc":"2.0","method":"agent.response",...}``
        as a WebSocket text frame after a workflow completes.  The background
        reader task places such notifications on an internal queue that this
        method reads from.
        """
        try:
            queue = (
                self._response_queues.setdefault(dialogue_id, asyncio.Queue())
                if dialogue_id
                else self._notification_queue
            )
            msg = await asyncio.wait_for(queue.get(), timeout=timeout)
            params = msg.get("params", {})
            if dialogue_id and bool(params.get("final", False)):
                self._response_queues.pop(dialogue_id, None)
            return params
        except asyncio.TimeoutError as exc:
            raise DaemonTimeoutError(
                f"timeout waiting for agent.response after {timeout}s",
                code=-32000,
            ) from exc

    def discard_response_queue(self, dialogue_id: str) -> None:
        """Stop tracking a dialogue after its caller has timed out or aborted."""
        if dialogue_id:
            self._response_queues.pop(dialogue_id, None)

    async def session_list(self, agent_id: str, user_id: str = "") -> list[dict[str, Any]]:
        return await self.send_request("session.list", {"agent_id": agent_id, "user_id": user_id})

    async def session_destroy(self, agent_id: str, session_id: str) -> dict[str, Any]:
        return await self.send_request(
            "session.destroy",
            {"agent_id": agent_id, "session_id": session_id},
        )

    async def session_clear_dialogue(
        self,
        agent_id: str,
        session_id: str,
        dialogue_id: str = "",
    ) -> dict[str, Any]:
        return await self.send_request(
            "session.clear_dialogue",
            {
                "agent_id": agent_id,
                "session_id": session_id,
                "dialogue_id": dialogue_id,
            },
        )

    async def workflow_list(self, agent_id: str) -> list[dict[str, Any]]:
        return await self.send_request("workflow.list", {"agent_id": agent_id})

    async def workflow_info(self, agent_id: str, workflow_id: str) -> dict[str, Any]:
        return await self.send_request(
            "workflow.info",
            {"agent_id": agent_id, "workflow_id": workflow_id},
        )

    async def runtime_status(self) -> dict[str, Any]:
        return await self.send_request("runtime.status", {})

    # ------------------------------------------------------------------
    # Background WebSocket reader
    # ------------------------------------------------------------------

    async def _ws_reader(self) -> None:
        """Read WebSocket text frames and dispatch to pending futures."""
        ws = self._ws
        if ws is None:
            return

        try:
            async for raw in ws:
                try:
                    message = json.loads(raw)
                except json.JSONDecodeError:
                    logger.warning("non-JSON frame from daemon: %r", raw[:200])
                    continue
                if not isinstance(message, dict):
                    logger.warning("non-object frame from daemon: %r", message)
                    continue
                await self._handle_message(message)

        except websockets.exceptions.ConnectionClosed:
            logger.info("daemon WebSocket closed")
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            logger.exception("ws reader error: %s", exc)
        finally:
            self._connected = False
            self._fail_all_pending(
                DaemonConnectionError("daemon WebSocket closed")
            )

    async def _handle_message(self, message: dict) -> None:
        """Resolve a single parsed JSON-RPC message (response or notification)."""
        # Notifications (no id) — push to queue for wait_for_response to consume
        if "id" not in message:
            event = message.get("method")
            if event in ("agent.response",):
                params = message.get("params", {})
                metadata = params.get("metadata", {}) if isinstance(params, dict) else {}
                dialogue_id = (
                    metadata.get("dialogue_id", "")
                    if isinstance(metadata, dict)
                    else ""
                )
                response_queues = getattr(self, "_response_queues", {})
                if dialogue_id:
                    queue = response_queues.get(dialogue_id)
                    if queue is None:
                        logger.info(
                            "dropping late or unregistered notification: method=%s dialogue_id=%s",
                            event,
                            dialogue_id,
                        )
                    else:
                        logger.debug(
                            "queuing notification: method=%s dialogue_id=%s",
                            event,
                            dialogue_id,
                        )
                        await queue.put(message)
                else:
                    logger.debug("queuing uncorrelated notification: method=%s", event)
                    await self._notification_queue.put(message)
            else:
                logger.debug("ignoring notification from daemon: %r", message)
            return

        req_id_raw: Any = message.get("id")
        if not isinstance(req_id_raw, int):
            logger.debug("dropping frame with non-integer id=%r", req_id_raw)
            return
        future = self._pending.pop(req_id_raw, None)
        if future is None or future.done():
            return
        future.set_result(message)

    # ------------------------------------------------------------------
    # Internals
    # ------------------------------------------------------------------

    @staticmethod
    def _encode_request(request_id: int, method: str, params: dict[str, Any]) -> str:
        """Build the wire-format JSON-RPC 2.0 request."""
        return json.dumps(
            {
                "jsonrpc": "2.0",
                "method": method,
                "params": params,
                "id": request_id,
            },
            separators=(",", ":"),
            ensure_ascii=False,
        )

    @staticmethod
    def _build_daemon_error(error_obj: Any, method: str) -> DaemonError:
        """Map a JSON-RPC ``error`` object onto the right Python exception."""
        if isinstance(error_obj, dict):
            code = error_obj.get("code")
            message = error_obj.get("message", "unknown daemon error")
            data = error_obj.get("data")
        else:
            code = None
            message = str(error_obj)
            data = None
        full_message = f"{method!r} failed: {message}"
        if data is not None:
            full_message = f"{full_message} (data={data!r})"
        cls = _ERROR_MAP.get(int(code), DaemonError) if code is not None else DaemonError
        try:
            return cls(full_message, code=int(code) if code is not None else None)
        except (TypeError, ValueError):
            return cls(full_message, code=None)

    def _fail_all_pending(self, exc: BaseException) -> None:
        """Fail every in-flight request with ``exc``."""
        for future in self._pending.values():
            if not future.done():
                future.set_exception(exc)
        self._pending.clear()


__all__ = [
    "DaemonClient",
    "DaemonError",
    "DaemonConnectionError",
    "DaemonTimeoutError",
    "InvalidParamsError",
    "NotFoundError",
]
