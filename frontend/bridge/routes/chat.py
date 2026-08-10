"""AgentFlow Bridge — WebSocket streaming chat endpoint.

Exposes ``/ws/chat/{agent_id}`` for streaming agent conversations to the
Vue frontend. The protocol is a thin envelope around the daemon's
``agent.execute`` / notification-pair:

* Client sends ``{"user_id": "...", "client_id": "...", "content": "..."}`` to start a turn.
* Bridge calls ``agent.execute`` to obtain a ``task_id``.
* Bridge polls ``wait_for_response`` until the daemon reports done or the
  client sends ``{"type": "stop"}``.
* Each poll result becomes a ``chunk`` frame; the terminal frame is ``done``.

Key design decision: ``session_id`` is a daemon-internal concept. The client
identifies itself via ``user_id`` + ``client_id``; the daemon maps these
internally to a session via its ``SessionRouter``.
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
import uuid
from datetime import datetime, timezone
from typing import Any

from fastapi import APIRouter, WebSocket, WebSocketDisconnect, status

from client import DaemonClient, DaemonError
from config import settings
from content_serializer import prepare_content_for_daemon
from models import ChatChunk, ChatDone, ChatRequest, ContentPart
from providers import SessionProvider
from routes.conversations import get_store, ConversationRecord
from routes.artifact_extractor import apply_artifact_output_mode, extract_artifact_infos, publicize_artifact_urls, serialize_artifacts
from services import _pull_and_save_traces

logger = logging.getLogger("agentflow.bridge.routes.chat")

router = APIRouter(tags=["chat"])

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

CLOSE_CODE_NORMAL = 1000
CLOSE_CODE_INTERNAL_ERROR = 1011

HEARTBEAT_SECONDS = 30
# ---------------------------------------------------------------------------
# Endpoint
# ---------------------------------------------------------------------------


@router.websocket("/ws/chat/{agent_id}")
async def chat_endpoint(websocket: WebSocket, agent_id: str) -> None:
    """Stream agent turns over WebSocket.

    Protocol summary (see module docstring for the full contract):

    * Inbound (client -> bridge): ``{"user_id": "...", "client_id": "...", "content": "..."}``
    * Inbound stop: ``{"type": "stop"}``
    * Outbound chunk: ``{"type": "chunk", "content": "...", "final": bool}``
    * Outbound done:  ``{"type": "done", "final": true}``
    * Outbound error: ``{"type": "error", "message": "...", "code": int}``

    The endpoint accepts the connection first, then validates ``agent_id``.
    Connection errors and daemon errors are forwarded to the frontend as
    ``error`` frames before closing the socket with a meaningful code.

    Phase-1 (Bridge session persistence): history context is injected from
    the configured ``SessionProvider`` before each turn, following the same
    messages-list protocol as ``chat_rest.py``.
    """
    await websocket.accept()

    daemon_client: DaemonClient | None = getattr(
        websocket.app.state, "daemon_client", None
    )
    if daemon_client is None:
        logger.error(
            "chat_turn.init_error: DaemonClient not attached to app.state",
        )
        await _send_error_and_close(
            websocket,
            message="bridge is not fully initialized",
            code=CLOSE_CODE_INTERNAL_ERROR,
        )
        return

    # Read SessionProvider from app.state (set up in main.lifespan).
    provider: SessionProvider | None = getattr(
        websocket.app.state, "session_provider", None
    )

    if not agent_id or not agent_id.strip():
        await _send_error_and_close(
            websocket,
            message="agent_id path parameter is required",
            code=CLOSE_CODE_INTERNAL_ERROR,
        )
        return

    # Track user_id + client_id across multiple turns on the same connection.
    current_user_id: str | None = None
    current_client_id: str | None = None

    # ``stop_event`` is set when the frontend sends ``{"type": "stop"}``;
    # the polling loop inside ``_run_turn`` watches it and exits early.
    stop_event = asyncio.Event()

    logger.info("chat_turn.connect agent=%s peer=%s", agent_id, _peer(websocket))

    try:
        # Keep-alive: ping every HEARTBEAT_SECONDS so a half-dead peer is
        # detected quickly.
        heartbeat_task = asyncio.create_task(
            _heartbeat_loop(websocket), name="ws-chat.heartbeat"
        )

        async def run_one_turn(req: ChatRequest) -> None:
            stop_event.clear()
            await _run_turn(
                websocket=websocket,
                daemon_client=daemon_client,
                agent_id=agent_id,
                request=req,
                stop_event=stop_event,
                provider=provider,
            )

        turn_task: asyncio.Task[None] | None = None
        recv_task: asyncio.Task[dict[str, Any]] = asyncio.create_task(
            websocket.receive_json(), name="ws-chat.recv"
        )

        while True:
            pending: set[asyncio.Task[Any]] = {recv_task}
            if turn_task is not None:
                pending.add(turn_task)
            done, _ = await asyncio.wait(
                pending, return_when=asyncio.FIRST_COMPLETED
            )

            if turn_task is not None and turn_task in done:
                # Drain the recv task if it also finished.
                if recv_task in done:
                    recv_task = asyncio.create_task(
                        websocket.receive_json(), name="ws-chat.recv"
                    )
                try:
                    turn_task.result()  # re-raise if the turn crashed
                except DaemonError:
                    raise
                except Exception as exc:  # noqa: BLE001
                    logger.exception("chat_turn.error agent=%s task_crashed: %s", agent_id, exc)
                    await _send_error_and_close(
                        websocket,
                        message=f"internal error: {exc}",
                        code=CLOSE_CODE_INTERNAL_ERROR,
                    )
                    return
                turn_task = None
                continue

            # recv completed — process the new frame.
            try:
                inbound_raw = recv_task.result()
                if logger.isEnabledFor(logging.DEBUG):
                    preview = json.dumps(inbound_raw, ensure_ascii=False)[:1024]
                    logger.debug("ws_recv agent=%s data=%s", agent_id, preview)
            except WebSocketDisconnect:
                logger.info("chat_turn.disconnect agent=%s", agent_id)
                if turn_task is not None:
                    stop_event.set()
                    turn_task.cancel()
                break

            inbound = _coerce_inbound(inbound_raw)

            # Stop signal: set the flag so the polling loop breaks, ack
            # with a done frame, but do NOT cancel the turn task.
            if isinstance(inbound, _StopSignal):
                logger.debug("chat_turn.stop agent=%s", agent_id)
                if turn_task is not None:
                    stop_event.set()
                await _safe_send(websocket, ChatDone().model_dump())
                recv_task = asyncio.create_task(
                    websocket.receive_json(), name="ws-chat.recv"
                )
                continue

            # Heartbeat ping — silently ignore.
            if isinstance(inbound, _PingSignal):
                recv_task = asyncio.create_task(
                    websocket.receive_json(), name="ws-chat.recv"
                )
                continue

            if not isinstance(inbound, ChatRequest):
                await _safe_send(
                    websocket,
                    _error_frame(
                        "invalid message: expected object with 'user_id', 'client_id', 'content' fields",
                        code=-32600,
                    ),
                )
                recv_task = asyncio.create_task(
                    websocket.receive_json(), name="ws-chat.recv"
                )
                continue

            # Reject concurrent execute: only one turn may run at a time.
            if turn_task is not None and not turn_task.done():
                logger.info(
                    "chat_turn.reject agent=%s user=%s (concurrent turn rejected)",
                    agent_id, inbound.user_id,
                )
                await _safe_send(
                    websocket,
                    _error_frame(
                        "another turn is already in flight",
                        code=-32000,
                    ),
                )
                recv_task = asyncio.create_task(
                    websocket.receive_json(), name="ws-chat.recv"
                )
                continue

            # Track user_id + client_id for the connection.
            current_user_id = inbound.user_id
            current_client_id = inbound.client_id

            logger.info(
                "chat_turn.start agent=%s user=%s client=%s content_len=%d",
                agent_id, inbound.user_id, inbound.client_id, len(inbound.content),
            )

            turn_task = asyncio.create_task(
                run_one_turn(inbound), name="ws-chat.turn"
            )
            recv_task = asyncio.create_task(
                websocket.receive_json(), name="ws-chat.recv"
            )
    except WebSocketDisconnect:
        logger.info("chat_turn.disconnect agent=%s (client closed)", agent_id)
    except DaemonError as exc:
        logger.warning(
            "chat_turn.error agent=%s code=%s msg=%s",
            agent_id,
            getattr(exc, "code", None),
            exc,
        )
        await _send_error_and_close(
            websocket,
            message=f"daemon error: {exc}",
            code=getattr(exc, "code", None) or CLOSE_CODE_INTERNAL_ERROR,
        )
    except Exception as exc:  # noqa: BLE001 — last-resort safety net
        logger.exception("chat_turn.error agent=%s unexpected: %s", agent_id, exc)
        await _send_error_and_close(
            websocket,
            message=f"internal error: {exc}",
            code=CLOSE_CODE_INTERNAL_ERROR,
        )
    finally:
        await _cleanup(websocket, agent_id)


# ---------------------------------------------------------------------------
# Internals
# ---------------------------------------------------------------------------


async def _run_turn(
    *,
    websocket: WebSocket,
    daemon_client: DaemonClient,
    agent_id: str,
    request: ChatRequest,
    stop_event: asyncio.Event,
    provider: SessionProvider | None = None,
) -> None:
    """Execute a single turn: agent.execute -> poll loop -> done frame.

    Uses ``user_id`` + ``client_id`` from the request to identify the session
    (daemon internally maps these to a session via SessionRouter).

    Phase-1 (Bridge session persistence): if ``provider`` is supplied, history
    context is injected from the SessionProvider before the turn, following the
    same messages-list protocol as ``chat_rest.py`` (§2.4 / §3.1).
    Persist to the provider after the turn completes.
    """
    # Generate unique dialogue_id per turn.
    dialogue_id = (
        f"{datetime.now(timezone.utc).strftime('%m%d%H%M%S')}"
        f"-{uuid.uuid4().hex[:12]}"
    )
    turn_timeout = settings.chat_turn_timeout_seconds

    # ── 上下文注入（设计文档 §2.4 / §3.1）──────────────────────
    logger = logging.getLogger("agentflow.bridge.routes.chat")
    history_messages: list[dict] = []
    bsession: dict | None = None
    if provider is not None:
        try:
            bsession = await provider.get_or_create_session(
                request.user_id, request.client_id,
            )
            ctx = await provider.get_context(
                bsession["id"], request.user_id,
                query=_content_to_text(request.content),
                limit_rounds=settings.session_context_limit_rounds,
            )
            history_messages = ctx.messages
            if ctx.memories:
                memory_text = "用户相关信息：\n" + "\n".join(ctx.memories)
                history_messages = [
                    {"role": "system", "content": memory_text},
                    *history_messages,
                ]
            logger.info(
                "ws_chat.context agent=%s session=%s history_msgs=%d memories=%d",
                agent_id, bsession["id"], len(history_messages), len(ctx.memories),
            )
        except Exception as exc:  # noqa: BLE101
            logger.warning(
                "ws_chat.context_failed agent=%s dialogue=%s: %s (proceeding without history)",
                agent_id, dialogue_id, exc,
            )

    this_turn_text = prepare_content_for_daemon(request.content, agent_id=agent_id,
                                                 user_id=request.user_id, client_id=request.client_id)
    messages_payload = (
        [*history_messages, {"role": "user", "content": this_turn_text}]
        if history_messages
        else [{"role": "user", "content": this_turn_text}]
    )
    daemon_content = json.dumps({
        "stream": False,
        "messages": messages_payload,
    }, ensure_ascii=False)

    try:
        execute_result = await asyncio.wait_for(
            daemon_client.agent_execute(
                agent_id,
                session_id="",
                content=daemon_content,
                user_id=request.user_id,
                client_id=request.client_id,
                dialogue_id=dialogue_id,
            ),
            timeout=turn_timeout,
        )
    except asyncio.TimeoutError:
        await _safe_send(
            websocket,
            _error_frame(
                f"agent.execute timed out after {turn_timeout}s",
                code=-32000,
            ),
        )
        daemon_client.discard_response_queue(dialogue_id)
        await _safe_send(websocket, ChatDone().model_dump())
        return
    except DaemonError:
        raise

    task_id = _extract_task_id(execute_result)
    if not task_id:
        daemon_client.discard_response_queue(dialogue_id)
        await _safe_send(
            websocket,
            _error_frame(
                "agent.execute did not return a task_id",
                code=-32000,
            ),
        )
        await _safe_send(websocket, ChatDone().model_dump())
        return

    # Poll for response notifications.
    final_payload: dict[str, Any] | None = None
    deadline = asyncio.get_running_loop().time() + turn_timeout
    chunk_count = 0
    turn_start = time.time()
    chunks_accumulated: list[str] = []
    # 最终 chunk 的转换后产物元数据（含 placeholder/data_base64，供持久化与回放一致）
    final_artifacts: list[dict] = []

    while True:
        if stop_event.is_set():
            logger.info(
                "chat_turn.abort agent=%s dialogue=%s task=%s elapsed=%.1fs chunks=%d",
                agent_id, dialogue_id, task_id, time.time() - turn_start, chunk_count,
            )
            # Notify daemon to interrupt the in-flight dialogue so its
            # session-level serialization lock is released and the next
            # queued dialogue can proceed.  Best-effort: failures are
            # logged but do not block the local abort path.
            daemon_session_id = ""
            if isinstance(execute_result, dict):
                daemon_session_id = execute_result.get("session_id") or ""
            try:
                await daemon_client.agent_send_control(
                    agent_id=agent_id,
                    command="DIALOGUE-INTERRUPT",
                    session_id=daemon_session_id,
                    user_id=request.user_id,
                    dialogue_id=dialogue_id,
                )
            except Exception as exc:
                logger.warning(
                    "chat_turn.abort.send_control_failed agent=%s dialogue=%s err=%s",
                    agent_id, dialogue_id, exc,
                )
            daemon_client.discard_response_queue(dialogue_id)
            break

        remaining = deadline - asyncio.get_running_loop().time()
        if remaining <= 0:
            logger.warning(
                "chat_turn.timeout agent=%s dialogue=%s task=%s timeout=%ss elapsed=%.1fs",
                agent_id, dialogue_id, task_id, turn_timeout, time.time() - turn_start,
            )
            await _safe_send(
                websocket,
                _error_frame(
                    f"turn timed out after {turn_timeout}s",
                    code=-32000,
                ),
            )
            daemon_client.discard_response_queue(dialogue_id)
            break

        try:
            # Race wait_for_response against stop_event so a client stop
            # interrupts the poll immediately instead of waiting for the
            # daemon's response timeout.
            wait_task = asyncio.create_task(
                daemon_client.wait_for_response(
                    agent_id, dialogue_id=dialogue_id, timeout=remaining,
                )
            )
            stop_task = asyncio.create_task(stop_event.wait())
            done, pending = await asyncio.wait(
                {wait_task, stop_task},
                timeout=remaining,
                return_when=asyncio.FIRST_COMPLETED,
            )
            for p in pending:
                p.cancel()
            if stop_task in done or stop_event.is_set():
                # Stop fired — fall through to the stop check at loop top.
                continue
            if wait_task not in done:
                # asyncio.wait timeout — retry.
                raise asyncio.TimeoutError()
            notification = wait_task.result()
        except asyncio.CancelledError:
            raise
        except asyncio.TimeoutError as exc:
            logger.debug(
                "chat_turn.poll_retry agent=%s dialogue=%s task=%s remaining=%.1fs",
                agent_id, dialogue_id, task_id, remaining,
            )
            await _safe_send(
                websocket,
                _error_frame(
                    f"turn timed out after {turn_timeout}s: {exc}",
                    code=-32000,
                ),
            )
            daemon_client.discard_response_queue(dialogue_id)
            break
        except DaemonError as exc:
            logger.error(
                "chat_turn.error agent=%s dialogue=%s task=%s error=%s",
                agent_id, dialogue_id, task_id, exc,
            )
            await _safe_send(
                websocket,
                _error_frame(f"daemon error while waiting for response: {exc}", code=-32000),
            )
            daemon_client.discard_response_queue(dialogue_id)
            break

        if not notification:
            continue

        is_final = _extract_final(notification)

        # ── 检测 daemon 侧的执行错误 ────────────────────────────
        # workflow 失败时，daemon 将错误详情写入 metadata.skill_result
        # 格式: {"status":"failed","error":"Command execution timeout"}
        if is_final:
            final_payload = notification
            err_msg = _extract_workflow_error(notification)
            if err_msg:
                logger.warning(
                    "chat_turn.workflow_error agent=%s dialogue=%s task=%s "
                    "elapsed=%.1fs error=%s",
                    agent_id, dialogue_id, task_id,
                    time.time() - turn_start, err_msg,
                )
                await _safe_send(
                    websocket,
                    _error_frame(err_msg, code=-32000),
                )
                break

        chunk_content = publicize_artifact_urls(_extract_content(notification))

        # ── Unified type field ────────────────────────────────────
        # daemon 通知携带 type 字段: text | llm_chunk | progress
        # text:      完整响应（非流式/流式结束时）, 直接作为 content
        # llm_chunk: OpenAI SSE 格式的 choices JSON, content 原样透传
        # progress:  进度通知, 标记 progress=True
        msg_type = _extract_metadata_value(notification, "type", "")
        is_progress = (msg_type == "progress")
        is_llm_chunk = (msg_type == "llm_chunk")

        chunk_count += 1
        artifacts = extract_artifact_infos(chunk_content) if is_final else []
        # 产物输出模式转换（artifact_reference / artifact_payload 两维正交开关）：
        # 仅 final chunk 应用（产物 URL 只出现在最终完整文本）；默认 link+url 时无转换，
        # 与历史行为完全一致（I-3 三路一致：WebSocket / SSE / 历史回放共用同一转换后结果）。
        if is_final:
            chunk_content, artifacts = apply_artifact_output_mode(chunk_content, artifacts)
        chunks_accumulated.append(chunk_content if msg_type == "text" else "")
        final_artifacts = [a.model_dump() for a in artifacts] if is_final else final_artifacts
        chunk = ChatChunk(content=chunk_content, final=is_final,
                          progress=is_progress, artifacts=artifacts,
                          type="chunk",
                          chunk_type=msg_type if msg_type in ("llm_chunk", "progress", "text") else "",
                          progress_skill=_extract_metadata_value(notification, "skill", ""),
                          progress_round=_extract_metadata_value(notification, "round", ""),
                          progress_status=_extract_metadata_value(notification, "status", ""))
        # 注入 `_` 前缀字段 — 与 SSE 路径一致，守设计文档 §3.2 统一字段命名
        chunk_dict = chunk.model_dump()
        chunk_dict["_chunk_type"] = chunk.chunk_type
        chunk_dict["_final"] = chunk.final
        chunk_dict["_progress"] = chunk.progress
        # _source 来自 metadata.source（coordinator/skill_executor），非技能名
        chunk_dict["_source"] = _extract_metadata_value(notification, "source", "")
        chunk_dict["_skill"] = chunk.progress_skill
        chunk_dict["_round"] = chunk.progress_round
        chunk_dict["_status"] = chunk.progress_status
        await _safe_send(websocket, chunk_dict)

        if is_final and not is_progress:
            # Defensive guard: a progress notification should never end a turn,
            # even if the daemon mistakenly marks it as final.  Progress updates
            # are intermediate — the real final chunk will follow once the
            # coordinator node actually completes.
            break
        if is_final and is_progress:
            logger.warning(
                "chat_turn.defense agent=%s dialogue=%s task=%s "
                "progress chunk marked final=False — ignoring final flag",
                agent_id, dialogue_id, task_id,
            )

    elapsed = time.time() - turn_start
    logger.info(
        "chat_turn.complete agent=%s dialogue=%s task=%s elapsed=%.1fs chunks=%d final=%s",
        agent_id, dialogue_id, task_id, elapsed, chunk_count, bool(final_payload),
    )

    # Persist to conversation store so the frontend can list past
    # conversations independently of the daemon's dialogue lifecycle.
    if chunks_accumulated:
        get_store().save(ConversationRecord(
            agent_id=agent_id,
            user_id=request.user_id,
            client_id=request.client_id,
            dialogue_id=dialogue_id,
            content="".join(chunks_accumulated),
        ))

    # ── 持久化到 Bridge SessionProvider（Phase 1）──────────────
    if provider is not None and chunks_accumulated:
        agentflow_session_id = ""
        if isinstance(execute_result, dict):
            agentflow_session_id = execute_result.get("session_id") or ""
        try:
            sess = bsession if bsession is not None else await provider.get_or_create_session(
                request.user_id, request.client_id,
            )
            await provider.update_agentflow_session_ref(sess["id"], agentflow_session_id)
            full_content = "".join(chunks_accumulated)
            # 持久化使用转换后产物元数据（含 placeholder/data_base64，I-3 三路一致）；
            # 默认模式（link+url）下 final_artifacts 即 extract 结果，行为与历史一致。
            artifacts_json = (
                json.dumps(final_artifacts, ensure_ascii=False)
                if final_artifacts else serialize_artifacts(full_content)
            )
            await provider.save_dialogue(
                session_id=sess["id"],
                user_input=_content_to_text(request.content),
                agent_response=full_content,
                dialogue_id=dialogue_id,
                agent_name=agent_id,
                artifacts=artifacts_json,
                images=_content_images(request.content),
            )
            logger.info(
                "ws_chat.trace_pull agent=%s dialogue=%s user=%s client=%s sess=%s",
                agent_id, dialogue_id, request.user_id, request.client_id, sess["id"],
            )
            await _pull_and_save_traces(
                daemon_client, provider,
                agent_id=agent_id,
                user_id=request.user_id,
                client_id=request.client_id,
                dialogue_id=dialogue_id,
                session_id=sess["id"],
            )
        except Exception as exc:  # noqa: BLE101
            logger.warning(
                "ws_chat.persist_failed agent=%s dialogue=%s: %s",
                agent_id, dialogue_id, exc,
            )

    await _safe_send(websocket, ChatDone().model_dump())


async def _heartbeat_loop(websocket: WebSocket) -> None:
    """Send a ping frame every ``HEARTBEAT_SECONDS`` seconds."""
    while True:
        await asyncio.sleep(HEARTBEAT_SECONDS)
        try:
            await websocket.send_json({"type": "ping"})
        except Exception:
            break


def _coerce_inbound(raw: Any) -> ChatRequest | _StopSignal | _PingSignal | None:
    """Parse an inbound WebSocket JSON message into the appropriate type."""
    if not isinstance(raw, dict):
        return None

    # Stop signal.
    if raw.get("type") == "stop":
        return _StopSignal()

    # Ping signal.
    if raw.get("type") == "ping":
        return _PingSignal()

    # ChatRequest: must have user_id, client_id, and content (string or array).
    user_id = raw.get("user_id")
    client_id = raw.get("client_id")
    content = raw.get("content")
    if isinstance(user_id, str) and isinstance(client_id, str) and (
        isinstance(content, str) or isinstance(content, list)
    ):
        return ChatRequest(user_id=user_id, client_id=client_id, content=content)

    return None


class _StopSignal:
    """Sentinel for a ``{"type": "stop"}`` inbound frame."""


class _PingSignal:
    """Sentinel for a ``{"type": "ping"}`` inbound frame."""


async def _send_error_and_close(
    websocket: WebSocket,
    *,
    message: str,
    code: int,
) -> None:
    """Send an error frame and then close the WebSocket."""
    try:
        await _safe_send(websocket, _error_frame(message, code=code))
        await websocket.close(code=code)
    except Exception:
        pass


def _error_frame(message: str, *, code: int) -> dict[str, Any]:
    return {"type": "error", "message": message, "code": code}


async def _safe_send(websocket: WebSocket, payload: dict[str, Any]) -> None:
    """Best-effort send; swallow errors so a single failed send doesn't crash the loop."""
    if logger.isEnabledFor(logging.DEBUG):
        preview = json.dumps(payload, ensure_ascii=False)[:1024]
        logger.debug("ws_send type=%s preview=%s", payload.get("type", "?"), preview)
    try:
        await websocket.send_json(payload)
    except Exception:
        pass


def _peer(websocket: WebSocket) -> str:
    """Return a human-readable peer identifier."""
    try:
        client = websocket.client
        if client and client.host:
            return f"{client.host}:{client.port}"
    except Exception:
        pass
    return "unknown"


def _extract_task_id(execute_result: Any) -> str | None:
    """Pull ``message_id`` out of an ``agent.execute`` result payload."""
    if isinstance(execute_result, dict):
        return execute_result.get("task_id") or execute_result.get("message_id")
    return None


def _content_to_text(content: str | list[ContentPart]) -> str:
    """Flatten a chat request's ``content`` field to plain text.

    Mirrors ``routes.chat_rest._content_to_text`` — used for SessionProvider
    storage (user_input) and context-injection query text.
    """
    if isinstance(content, str):
        return content
    parts: list[str] = []
    for p in content:
        text = getattr(p, "text", None)
        if text:
            parts.append(text)
            continue
        url = getattr(p, "image_url", None)
        url_str = getattr(url, "url", "") if url else ""
        if url_str:
            if url_str.startswith("data:"):
                name = url_str.split(";")[0].replace("data:", "")[:20]
            else:
                name = url_str.rstrip("/").split("/")[-1] or "image"
        else:
            name = "image"
        parts.append(f"[image:{name}]")
    return "".join(parts)


def _content_images(content: str | list[ContentPart]) -> str:
    """从聊天请求 content 中提取用户附加图片的 data URI 列表（JSON 编码）。

    仅收集 ``data:`` 前缀的内联图片（blob URL / 文件路径在会话历史中无法恢复，
    落库无意义）；纯文本消息返回 ``"[]"``。供 save_dialogue 持久化用户图片。
    """
    if isinstance(content, str):
        return "[]"
    images: list[str] = []
    for p in content:
        url = getattr(p, "image_url", None)
        url_str = getattr(url, "url", "") if url else ""
        if url_str.startswith("data:"):
            images.append(url_str)
    return json.dumps(images, ensure_ascii=False)


def _extract_final(payload: Any) -> bool:
    """从通知 payload 中提取 final 标记。

    守设计文档 §3.2：``metadata.final`` 是唯一真相源（string "true"/"false"）。
    优先读 metadata.final，缺失时降级到顶层 ``final``（兼容 daemon 历史注入）。
    """
    if isinstance(payload, dict):
        # 尝试 metadata.final（设计文档 §3.2 唯一真相源）
        meta = payload.get("metadata")
        if isinstance(meta, dict):
            meta_final = meta.get("final")
            if meta_final == "true":
                return True
            if meta_final == "false":
                return False
        # 降级：顶层 final（daemon 历史注入的兼容 fallback）
        val = payload.get("final", False)
        if isinstance(val, bool):
            return val
        if isinstance(val, str):
            return val == "true"
        logger.warning(
            "_extract_final: unexpected type for top-level 'final': %s",
            type(val).__name__,
        )
        return False
    return False


def _extract_content(payload: Any) -> str:
    if isinstance(payload, dict):
        content = payload.get("content") or payload.get("text") or ""
        return str(content)
    return ""


def _extract_metadata_value(payload: Any, key: str, default: str = "") -> str:
    """Extract a value from a daemon notification's metadata dict."""
    if isinstance(payload, dict):
        meta = payload.get("metadata")
        if isinstance(meta, dict):
            val = meta.get(key)
            if val is not None:
                return str(val)
        elif isinstance(meta, str):
            try:
                parsed = json.loads(meta)
                if isinstance(parsed, dict):
                    val = parsed.get(key)
                    if val is not None:
                        return str(val)
            except json.JSONDecodeError:
                pass
    return default


def _extract_workflow_error(payload: Any) -> str:
    """Extract workflow-level error from a daemon notification, or empty string.

    When a workflow node fails (e.g. Command execution timeout), the runtime
    writes ``metadata.skill_result`` as a JSON blob::

        {"status":"failed","error":"Command execution timeout"}

    Returns the error string, or ``""`` if the notification carries no error.
    """
    if not isinstance(payload, dict):
        return ""
    metadata = payload.get("metadata")
    if not isinstance(metadata, dict):
        return ""
    skill_result_raw = metadata.get("skill_result", "")
    if not isinstance(skill_result_raw, str) or not skill_result_raw:
        return ""
    try:
        parsed = json.loads(skill_result_raw)
        if isinstance(parsed, dict) and parsed.get("status") == "failed":
            err = parsed.get("error", "")
            return str(err) if err else ""
    except (json.JSONDecodeError, TypeError, ValueError):
        pass
    return ""


async def _cleanup(websocket: WebSocket, agent_id: str) -> None:
    """Defensive cleanup when the endpoint exits."""
    try:
        await websocket.close(code=CLOSE_CODE_NORMAL)
    except Exception:
        pass


__all__ = ["router"]
