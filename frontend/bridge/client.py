"""AgentFlow Bridge — daemon JSON-RPC client over raw TCP.

Transport: raw TCP (newline-delimited JSON-RPC 2.0).
We use raw TCP instead of WebSocket because the daemon's ``send_notification()``
does not wrap push notifications in WebSocket frames, which corrupts the WS
stream. See ``docs/gaps/09-WebSocket推送.md``.

The wire format is:
  Request:  ``{jsonrpc..."method":"...","params":{...},"id":N}\n``
  Response: ``{jsonrpc..."result":...,"id":N}\n``
"""

from __future__ import annotations

import asyncio
import itertools
import json
import logging
from typing import Any, Awaitable, Callable, Optional

from config import BridgeSettings, settings as _default_settings

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Exceptions
# ---------------------------------------------------------------------------


class DaemonError(Exception):
    """Base class for every daemon-side error surfaced by ``DaemonClient``."""

    def __init__(self, message: str, *, code: Optional[int] = None) -> None:
        super().__init__(message)
        self.code = code
        self.message = message

    def __str__(self) -> str:
        return self.message


class DaemonConnectionError(DaemonError):
    """Raised when the underlying TCP connection cannot be opened."""


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
    """Raw-TCP JSON-RPC 2.0 client for the agentflowd daemon.

    Uses newline-delimited JSON over a plain TCP socket (not WebSocket).
    """

    def __init__(
        self,
        host: Optional[str] = None,
        port: Optional[int] = None,
        *,
        timeout: Optional[float] = None,
        bridge_settings: Optional[BridgeSettings] = None,
    ) -> None:
        self._bridge_settings = bridge_settings or _default_settings
        self._host = host or self._bridge_settings.daemon_host
        self._port = port or self._bridge_settings.daemon_port
        self._timeout = (
            timeout if timeout is not None else self._bridge_settings.request_timeout_seconds
        )

        self._id_counter = itertools.count(1)
        self._pending: dict[int, asyncio.Future[dict[str, Any]]] = {}
        self._notification_queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue()
        self._reader: Optional[asyncio.StreamReader] = None
        self._writer: Optional[asyncio.StreamWriter] = None
        self._connected = False
        self._closing = False
        self._reader_task: Optional[asyncio.Task[None]] = None

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------

    async def connect(self) -> None:
        """Open a raw TCP connection and start the reader task."""
        if self._connected:
            return
        if self._reader_task is not None and not self._reader_task.done():
            return
        # Prevent creating a second reader task while the existing one is
        # still alive (e.g. during its own reconnection logic).  The
        # background status-poll loop calls ``connect()``, and if the
        # reader loop is in the middle of reconnecting internally we must
        # not orphan a second reader on the same underlying stream.
        if self._reader_task is not None and not self._reader_task.done():
            return
        try:
            self._reader, self._writer = await asyncio.wait_for(
                asyncio.open_connection(self._host, self._port),
                timeout=self._timeout,
            )
        except Exception as exc:
            self._connected = False
            raise DaemonConnectionError(
                f"failed to connect to {self._host}:{self._port}: {exc}"
            ) from exc

        self._connected = True
        self._closing = False
        self._reader_task = asyncio.create_task(
            self._reader_loop(), name="DaemonClient.reader"
        )
        logger.info("DaemonClient connected %s:%s", self._host, self._port)

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

        if self._writer is not None:
            try:
                self._writer.close()
                await self._writer.wait_closed()
            except Exception:
                logger.debug("ignoring close() error", exc_info=True)

        self._reader = None
        self._writer = None
        self._connected = False
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

        Serialises the request as newline-delimited JSON over the raw TCP socket.
        """
        if not self._connected or self._writer is None:
            raise DaemonConnectionError("client is not connected")

        request_id = next(self._id_counter)
        loop = asyncio.get_running_loop()
        future: asyncio.Future[dict[str, Any]] = loop.create_future()
        self._pending[request_id] = future

        frame = self._encode_request(request_id, method, params or {})
        try:
            self._writer.write(frame.encode("utf-8"))
            self._writer.write(b"\n")
            await self._writer.drain()
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
    ) -> dict[str, Any]:
        params: dict[str, Any] = {
            "agent_id": agent_id,
            "user_id": user_id or "bridge-user",
            "client_id": client_id,
            "content": content,
        }
        if dialogue_id:
            params["dialogue_id"] = dialogue_id
        elif session_id:
            params["dialogue_id"] = session_id
        return await self.send_request("agent.execute", params)

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
        timeout: float = 30.0,
    ) -> dict[str, Any]:
        """Wait for an ``agent.response`` notification pushed by the daemon.

        The daemon pushes ``{\"jsonrpc\":\"2.0\",\"method\":\"agent.response\",...}``
        as a newline-delimited JSON line on the same TCP connection after a
        workflow completes. The background reader loop places such notifications
        on an internal queue that this method reads from.
        """
        try:
            msg = await asyncio.wait_for(
                self._notification_queue.get(), timeout=timeout
            )
            return msg.get("params", {})
        except asyncio.TimeoutError as exc:
            raise DaemonTimeoutError(
                f"timeout waiting for agent.response after {timeout}s",
                code=-32000,
            ) from exc

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
    # Background reader loop
    # ------------------------------------------------------------------

    async def _reader_loop(self) -> None:
        """Read newline-delimited JSON responses and dispatch to pending futures."""
        retry_delay = 1.0
        buf = b""
        while not self._closing:
            if self._reader is None:
                # Attempt reconnection instead of sleeping forever.
                await asyncio.sleep(retry_delay)
                retry_delay = min(retry_delay * 2, 30.0)
                try:
                    self._reader, self._writer = await asyncio.wait_for(
                        asyncio.open_connection(self._host, self._port),
                        timeout=self._timeout,
                    )
                    self._connected = True
                    retry_delay = 1.0
                    logger.info("DaemonClient reconnected %s:%s", self._host, self._port)
                except Exception:
                    self._reader = None
                    self._writer = None
                continue

            try:
                while True:
                    line = await asyncio.wait_for(
                        self._reader.readline(), timeout=self._timeout * 2
                    )
                    if not line:
                        logger.info("daemon connection closed")
                        break
                    text = line.decode("utf-8").strip()
                    if not text:
                        continue
                    await self._dispatch_frame(text)
                # Connection closed — break out to reconnect
                self._connected = False
                self._fail_all_pending(
                    DaemonConnectionError("daemon connection closed")
                )
            except asyncio.TimeoutError:
                # No data for a while — the connection may still be alive;
                # send a health check to probe liveness.
                try:
                    if self._writer is not None:
                        await self.health()
                except Exception:
                    logger.debug("liveness probe failed", exc_info=True)
                continue
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                logger.exception("reader loop crashed: %s", exc)
                self._connected = False
                self._fail_all_pending(
                    DaemonConnectionError(f"reader loop error: {exc}")
                )

            # Reconnection
            logger.info("reconnecting in %.1fs", retry_delay)
            await asyncio.sleep(retry_delay)
            retry_delay = min(retry_delay * 2, 30.0)
            try:
                self._reader, self._writer = await asyncio.wait_for(
                    asyncio.open_connection(self._host, self._port),
                    timeout=self._timeout,
                )
                self._connected = True
                retry_delay = 1.0
                logger.info("DaemonClient reconnected %s:%s", self._host, self._port)
            except Exception as exc:
                logger.warning("reconnect failed: %s", exc)
                self._reader = None
                self._writer = None

    async def _dispatch_frame(self, text: str) -> None:
        """Parse a single JSON-RPC response line and resolve the matching future."""
        try:
            message = json.loads(text)
        except (TypeError, ValueError) as exc:
            logger.warning("malformed frame from daemon: %r (%s)", text, exc)
            return
        if not isinstance(message, dict):
            logger.warning("non-object frame from daemon: %r", message)
            return

        # Notifications (no id) — push to queue for wait_for_response to consume
        if "id" not in message:
            event = message.get("method")
            if event in ("agent.response",):
                logger.debug("queuing notification: method=%s", event)
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