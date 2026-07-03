"""AISF SysAgent 调度入口 — 接收上游 SysAgent 的 TaskAssignment 请求。

对外暴露 ``POST /v1/tasks``（经 main.py 的 ``app.mount("/health", bridge_app)``
挂载后，完整路径为 ``/health/v1/tasks``），即 AISF 协议规定的 Bridge 被调度端点。

工作模式（默认 async）：
  1. 接收 AccessCtrl 请求体（PascalCase）
  2. 解析 → 建 HaContext → 立即返回 202 accepted
  3. 后台执行 agentflowd 任务（复用 app.state.daemon_client 长连接）
  4. 完成后主动 POST 回上游 ``http://{SysAgentEntity}:23767/coremind/systemagent/v1/callback``
     - 先发 TaskStatusReport (code=0 完成 / 500 失败)
     - 再发 UserOutput (最终文本)

对标 HealthAgent 的 TaskRouter + HaContext + ForwardToExternalSysAgent。
协议字段定义见 docs/contracts/2.AISF-interface.md。
"""

from __future__ import annotations

import asyncio
import logging
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
)
from client import DaemonClient
from config import settings

logger = logging.getLogger("routes.aisf_tasks")

router = APIRouter(prefix="/v1", tags=["aisf-tasks"])

# 进程级 HaContext 存储（按上游 TaskID 索引）
_ha_store = HaContextStore()


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
        "→ /v1/tasks: eventType=%s taskID=%s sender=%s input=%s",
        event_type, task_id, header.get("sender", ""),
        parsed.get("data", {}).get("userInputText", "")[:80],
    )

    # 仅处理 TASKASSIGNMENT；其他事件类型暂不支持
    if event_type != "TASKASSIGNMENT":
        logger.warning(
            "unsupported eventType=%s taskID=%s (only TASKASSIGNMENT handled)",
            event_type, task_id,
        )
        return JSONResponse(
            status_code=202,
            content={"status": "ignored", "task_id": task_id, "reason": f"eventType {event_type} not handled"},
        )

    daemon = getattr(request.app.state, "daemon_client", None)
    if daemon is None or not daemon.is_connected:
        logger.error("daemon client not available for taskID=%s", task_id)
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
    asyncio.create_task(
        _execute_and_callback(
            daemon, parsed, ctx, prev_dialogue_id, agent_id
        )
    )
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
        logger.info(
            "task done: taskID=%s dialogue_id=%s content_len=%d",
            task_id, dialogue_id, len(full_content),
        )

        # 回调上游：先 TaskStatusReport(0=完成)，再 UserOutput
        if ctx.sysAgentEntity:
            await http_post_to_upstream(
                ctx.sysAgentEntity,
                build_task_status_report(ctx, 0, "TaskCompleted"),
            )
            await http_post_to_upstream(
                ctx.sysAgentEntity,
                build_user_output(ctx, full_content),
            )
            logger.info("callback posted to upstream: taskID=%s", task_id)

        return {
            "status": "completed",
            "task_id": task_id,
            "dialogue_id": dialogue_id,
            "content_len": len(full_content),
        }

    except Exception as exc:
        logger.error(
            "task failed: taskID=%s dialogue_id=%s error=%s",
            task_id, dialogue_id, exc,
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
