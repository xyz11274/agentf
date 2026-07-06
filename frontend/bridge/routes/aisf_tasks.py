"""AISF SysAgent 调度入口 — 接收上游 SysAgent 的 TaskAssignment 请求。

对外暴露 ``POST /aisf/healthagent/v1/tasks``，即 AISF 协议规定的 Bridge 被调度端点。

工作模式（默认 async）：
  1. 接收 AccessCtrl 请求体（PascalCase）
  2. 解析 → 建 HaContext → 立即返回 202 accepted
  3. 后台执行 agentflowd 任务（复用 app.state.daemon_client 长连接）
  4. 完成后主动 POST 回上游 ``{scheme}://{SysAgentEntity}:23767/coremind/systemagent/v1/callback``
     - 先发 UserOutput (最终文本)
     - 再发 TaskStatusReport (code=200 完成 / 500 失败)

对标 HealthAgent 的 TaskRouter + HaContext + ForwardToExternalSysAgent。
协议字段定义见 docs/contracts/2.AISF-interface.md。
"""

from __future__ import annotations

import asyncio
import logging
import time
import uuid
from datetime import datetime, timezone
from typing import Any

from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse

from aisf_adapter.sysagent.server import (
    HaContext,
    HaContextStore,
    build_task_status_report,
    build_user_output,
    http_post_to_upstream,
    parse_accessctrl,
    post_result_to_upstream,
)
from client import DaemonClient
from config import settings

logger = logging.getLogger("routes.aisf_tasks")

router = APIRouter(prefix="/aisf/healthagent/v1", tags=["aisf-tasks"])

# 进程级 HaContext 存储（按上游 TaskID 索引）
_ha_store = HaContextStore()

# Per-(user, client) serialization queue.
# Key: f"{user_id}:{client_id}", Value: asyncio.Queue of pending tasks.
# Ensures that concurrent TaskAssignment requests for the same user+client
# are executed sequentially, preventing daemon session lock contention.
_serial_queues: dict[str, asyncio.Queue] = {}
_serial_queue_lock = asyncio.Lock()


async def _get_or_create_queue(user_id: str, client_id: str) -> asyncio.Queue:
    """Get or create a serialization queue for the given user+client pair."""
    key = f"{user_id}:{client_id}"
    async with _serial_queue_lock:
        if key not in _serial_queues:
            _serial_queues[key] = asyncio.Queue()
        return _serial_queues[key]


async def _serial_executor(user_id: str, client_id: str) -> None:
    """Background worker: drain the serial queue for one user+client pair.

    Processes tasks one at a time, in FIFO order.
    """
    key = f"{user_id}:{client_id}"
    queue = _serial_queues.get(key)
    if not queue:
        return
    while not queue.empty():
        coro = await queue.get()
        try:
            await coro
        except Exception as exc:
            logger.error("serial_executor.error user=%s client=%s err=%s", user_id, client_id, exc)
        finally:
            queue.task_done()
    # Clean up empty queue
    async with _serial_queue_lock:
        if key in _serial_queues and _serial_queues[key].empty():
            del _serial_queues[key]


@router.post("/tasks")
async def receive_task(request: Request) -> JSONResponse:
    """接收上游 SysAgent 的 TaskAssignment 请求。

    完全开放（无鉴权），按 AISF 系统内调度语义信任调用方。
    默认异步：立即返回 202，后台执行完成后回调上游。
    """
    try:
        raw = await request.json()
    except Exception as exc:
        logger.warning("invalid JSON body: %s", exc)
        return JSONResponse(
            status_code=400, content={"error": f"invalid JSON: {exc}"}
        )

    parsed = parse_accessctrl(raw)
    event_type = parsed.get("eventType", "UNKNOWN")
    header = parsed.get("header", {})
    task_id = header.get("taskID", "")

    logger.info(
        "fulfill_task.accept eventType=%s taskID=%s sender=%s user=%s input=%.80s",
        event_type, task_id, header.get("sender", ""),
        header.get("userName", ""), parsed.get("data", {}).get("userInputText", ""),
    )

    # 仅处理 TASKASSIGNMENT；其他事件类型暂不支持
    if event_type != "TASKASSIGNMENT":
        logger.warning(
            "fulfill_task.ignore eventType=%s taskID=%s (only TASKASSIGNMENT handled)",
            event_type, task_id,
        )
        return JSONResponse(
            status_code=202,
            content={"status": "ignored", "task_id": task_id, "reason": f"eventType {event_type} not handled"},
        )

    daemon = getattr(request.app.state, "daemon_client", None)
    if daemon is None or not daemon.is_connected:
        logger.error("fulfill_task.reject taskID=%s reason=daemon_unavailable", task_id)
        return JSONResponse(
            status_code=503,
            content={"error": "daemon_unavailable", "task_id": task_id},
        )

    # 构建 HaContext（保留上游元数据，用于回传报文构造）
    ctx = HaContext(
        sysAgentEntity=header.get("sysAgentEntity", ""),
        parentTaskID=task_id,
        sender=header.get("sender", ""),
        userName=header.get("userName", ""),
        deviceId=header.get("deviceId", ""),
        deviceCategory=header.get("deviceCategory", ""),
        uuid=header.get("uuid", ""),
        clientSessionId=header.get("clientSessionId", ""),
        acmSessionId=header.get("acmSessionId", ""),
        teamEnvironmentID=header.get("teamEnvironmentID", ""),
        eventTimeStamp=header.get("eventTimeStamp", ""),
        clientExField=header.get("clientExField", ""),
    )
    await _ha_store.save(task_id, ctx)

    # 查找同一用户上次 dialogue_id（多轮续对话）
    prev_dialogue_id = await _ha_store.get_last_dialogue_id(
        ctx.userName, ctx.deviceId
    )

    mode = settings.aisf_adapter_mode or "async"
    agent_id = settings.aisf_adapter_agent_id
    if mode == "sync":
        # 同步模式：等结果再返回（调试用）
        result = await _execute_and_callback(
            daemon, parsed, ctx, prev_dialogue_id, agent_id
        )
        return JSONResponse(status_code=200, content=result)

    # 异步模式（默认）：后台执行，立即返回 202
    # 但按 (userName, deviceId) 串行化排队，避免 daemon session 锁竞争
    queue = await _get_or_create_queue(ctx.userName, ctx.deviceId)
    await queue.put(
        _execute_and_callback(
            daemon, parsed, ctx, prev_dialogue_id, agent_id
        )
    )
    # 如果没有正在运行的 executor，启动一个
    asyncio.create_task(_serial_executor(ctx.userName, ctx.deviceId))
    return JSONResponse(
        status_code=202,
        content={"status": "accepted", "task_id": task_id, "mode": "async"},
    )


async def _execute_and_callback(
    daemon: DaemonClient,
    parsed: dict,
    ctx: HaContext,
    prev_dialogue_id: str,
    agent_id: str,
) -> dict[str, Any]:
    """执行 agentflowd 任务并回调上游。

    复用 app.state.daemon_client（长连接 WebSocket），对标 coremind_internal.py
    的 agent_execute + wait_for_response 循环。
    """
    task_id = ctx.parentTaskID
    user_input = parsed.get("data", {}).get("userInputText", "")
    user_id = ctx.userName or "sysagent-user"
    client_id = ctx.deviceId or "sysagent"

    # dialogue_id：复用旧对话 ID，否则生成新的
    dialogue_id = prev_dialogue_id or (
        f"{datetime.now(timezone.utc).strftime('%m%d%H%M%S')}"
        f"-{uuid.uuid4().hex[:12]}"
    )

    timeout = settings.aisf_adapter_timeout
    chunks: list[str] = []
    start_ts = time.time()

    logger.info(
        "fulfill_task.start taskID=%s dialogue=%s agent=%s user=%s input_len=%d",
        task_id, dialogue_id, agent_id, user_id, len(user_input),
    )

    try:
        await daemon.agent_execute(
            agent_id,
            session_id="",
            content=user_input,
            user_id=user_id,
            client_id=client_id,
            dialogue_id=dialogue_id,
            platform_context={},
            request_id=task_id,
        )

        # 轮询直到 final
        while True:
            response = await daemon.wait_for_response(
                agent_id, dialogue_id=dialogue_id, timeout=timeout
            )
            if isinstance(response, dict):
                content = response.get("content")
                if content:
                    chunks.append(str(content))
                if response.get("final") is True:
                    break
            else:
                break

        # 更新 HaContext 内部 ID（供后续多轮重入用）
        await _ha_store.update_dialogue_ids(task_id, dialogue_id, "")
        await _ha_store.set_last_dialogue_id(user_id, client_id, dialogue_id)

        full_content = "".join(chunks)
        elapsed = time.time() - start_ts
        logger.info(
            "fulfill_task.complete taskID=%s dialogue=%s elapsed=%.1fs content_len=%d chunks=%d",
            task_id, dialogue_id, elapsed, len(full_content), len(chunks),
        )

        # 回调上游：先 UserOutput，再 TaskStatusReport(code=200)
        if ctx.sysAgentEntity:
            cb_start = time.time()
            await post_result_to_upstream(ctx, full_content)
            logger.info(
                "fulfill_task.callback taskID=%s upstream=%s cb_elapsed=%.1fs",
                task_id, ctx.sysAgentEntity, time.time() - cb_start,
            )

        return {
            "status": "completed",
            "task_id": task_id,
            "dialogue_id": dialogue_id,
            "content_len": len(full_content),
            "elapsed": round(elapsed, 1),
        }

    except Exception as exc:
        elapsed = time.time() - start_ts
        logger.error(
            "fulfill_task.error taskID=%s dialogue=%s elapsed=%.1fs error=%s",
            task_id, dialogue_id, elapsed, exc,
        )
        # 失败回调上游
        if ctx.sysAgentEntity:
            try:
                await http_post_to_upstream(
                    ctx.sysAgentEntity,
                    build_task_status_report(ctx, 500, f"ExecutionFailed: {exc}"),
                )
            except Exception as cb_err:
                logger.error("failure callback also failed: taskID=%s %s", task_id, cb_err)
        return {
            "status": "error",
            "task_id": task_id,
            "dialogue_id": dialogue_id,
            "error": str(exc),
        }
