"""上游 CoreMind 平台 Mock 服务器 — 完全模拟 sidecar 上游回调链路。

工作模式（异步回调）：
  1. 前端 POST /send 触发请求
  2. Mock 构造 AccessCtrl（设 SysAgentEntity=自身地址），POST 到 sidecar
  3. Sidecar 返回 202 accepted，后台转发 Bridge
  4. Sidecar 逐块 POST 回调到 Mock 的 /coremind/systemagent/v1/callback
  5. 前端通过 GET /events (SSE) 实时接收回调流

端点：
  POST /send                        — 前端触发送请求
  POST /coremind/systemagent/v1/callback  — sidecar 回调入口
  GET  /events                      — SSE 流式回调事件
  GET  /health                      — 健康检查
"""

from __future__ import annotations

import asyncio
import json
import logging
import time
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, AsyncIterator

import httpx
import uvicorn
from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse, StreamingResponse

logger = logging.getLogger("upstream_mock")
# 模块级日志配置：uvicorn 以 import string 方式启动时也生效
logger.setLevel(logging.INFO)
if not logger.handlers:
    _handler = logging.StreamHandler()
    _handler.setFormatter(logging.Formatter(
        "%(asctime)s [%(levelname)s] upstream_mock: %(message)s"
    ))
    logger.addHandler(_handler)

app = FastAPI(title="CoreMind Upstream Mock")


# ── 请求/响应日志中间件 ──────────────────────────────────────────
@app.middleware("http")
async def log_requests(request: Request, call_next):
    """记录每个请求的方法、路径、请求体（预览）和响应状态码。"""

    # 读取请求体
    body_bytes = await request.body()
    body_str = body_bytes.decode("utf-8", errors="replace")

    response = await call_next(request)

    # 收集响应体
    resp_str = ""
    if not isinstance(response, StreamingResponse):
        try:
            # StreamingResponse 的 body 属性会抛 RuntimeError，需跳过
            resp_str = (response.body or b"").decode("utf-8", errors="replace")
        except (RuntimeError, AttributeError):
            resp_str = "<not available>"
    else:
        resp_str = "<streaming>"

    def _pretty(body: str) -> str:
        """JSON 缩进格式化，非 JSON 原样返回。"""
        if not body or body.startswith("<"):
            return body
        try:
            return json.dumps(json.loads(body), indent=2, ensure_ascii=False)
        except (json.JSONDecodeError, ValueError):
            return body

    logger.info(
        "[REQ] %s %s | body=\n%s",
        request.method, request.url.path, _pretty(body_str),
    )
    logger.info(
        "[RES] %s %s %d | body=\n%s",
        request.method, request.url.path, response.status_code, _pretty(resp_str),
    )
    return response


def _truncate_json(data: bytes, max_len: int) -> str:
    """将 bytes 解码为字符串，超长则截断并标记。"""
    s = data.decode("utf-8", errors="replace")
    if len(s) > max_len:
        return s[:max_len] + "... (truncated)"
    return s


# ── 配置 ─────────────────────────────────────────────────────────
SIDECAR_URL = "http://127.0.0.1:23767"
SIDECAR_CALLBACK_PATH = "/coremind/systemagent/v1/callback"
MOCK_HOST = "127.0.0.1"
MOCK_PORT = 23768
MOCK_CALLBACK_URL = f"http://{MOCK_HOST}:{MOCK_PORT}"

# ── 回调事件存储（进程内存，按 task_id 索引）────────────────────
@dataclass
class TaskSession:
    task_id: str
    dialogue_id: str = ""
    events: list[dict] = field(default_factory=list)
    done: bool = False
    created_at: float = field(default_factory=time.time)


_tasks: dict[str, TaskSession] = {}
_tasks_lock = asyncio.Lock()
"""task_id → TaskSession。sidecar 可能返回同一个 task_id 多次回调。"""

# SSE 订阅者队列
_sse_subscribers: dict[str, list[asyncio.Queue]] = {}
_sse_lock = asyncio.Lock()


async def _push_event(task_id: str, event: dict) -> None:
    """将回调事件推入存储和所有 SSE 订阅者。"""
    async with _tasks_lock:
        session = _tasks.get(task_id)
        if session:
            session.events.append(event)
            if event.get("type") == "done":
                session.done = True

        # 推给 SSE 订阅者
        queues = _sse_subscribers.get(task_id, [])
        for q in queues:
            await q.put(event)


# ── 端点 ─────────────────────────────────────────────────────────


@app.post("/send")
async def handle_send(request: Request) -> JSONResponse:
    """前端触发送指令——构造 AccessCtrl 请求，POST 到 sidecar。

    请求体: {text: str, images?: str[], agent_id?: str, user_id?: str, client_id?: str, device_id?: str}
    """
    body = await request.json()
    user_text = body.get("text", "")
    images = body.get("images", [])
    agent_id = body.get("agent_id", "outfit-assistant")
    user_id = body.get("user_id", "mock-upstream-user")
    client_session_id = body.get("client_id", f"sim-session-{uuid.uuid4().hex[:12]}")
    # DeviceId 与 ClientSessionId 是独立维度：DeviceId 标识物理设备，原样回传；
    # ClientSessionId 标识会话，与 UserName 共同构成身份映射对（对齐 sidecar
    # sidecar_router.cpp:191-199 的 user_name + client_session_id → bridge user_id + client_id）。
    # 不复用 client_session_id，否则抹平二者分离的契约，掩盖上游对设备维度的依赖。
    device_id = body.get("device_id", f"sim-device-{uuid.uuid4().hex[:12]}")
    task_id = f"sim_task_{datetime.now(timezone.utc).strftime('%m%d%H%M%S')}_{uuid.uuid4().hex[:8]}"

    # 构造 AccessCtrl 请求体（带 SysAgentEntity → 异步回调模式）
    # 消息类型组合对齐 sidecar router_logic.cpp is_supported_message_type（B1）：
    #   UserTaskManagement/TaskAssignment（任务下发）或 DataExchange/UserInput（用户输入）。
    # 不可用 DataExchange/TaskAssignment 混搭，否则 sidecar 返回 400 unsupported message type。
    accessctrl: dict[str, Any] = {
        "Header": {
            "MessageType": "UserTaskManagement",
            "Sender": "UpstreamMock",
            "Receiver": "SysAgent",
            "UserName": user_id,
            "DeviceId": device_id,
            "DeviceCategory": "2",
            "UUID": f"mock_{uuid.uuid4().hex[:12]}",
            "ClientSessionId": client_session_id,
            "TaskID": task_id,
            "DialogueId": f"dlg_{uuid.uuid4().hex[:12]}",
            "SysAgentEntity": f"{MOCK_HOST}:{MOCK_PORT}",
            "SeqNum": task_id,
            "EventTimeStamp": datetime.now(timezone.utc).isoformat(),
        },
        "Data": {
            "MessageType": "TaskAssignment",
            "UserInputText": user_text,
        },
    }

    # 如果有图片，加入 UserInputObject
    # CMF 协议中 UserInputObject 必须是 stringified JSON string（对齐 sidecar
    # router_logic.cpp build_openai_messages / sidecar_router.cpp:203）。
    # 传嵌套 JSON 对象会被 sidecar 的 asString() 取空串，多模态图片被静默丢弃。
    if images:
        accessctrl["Data"]["UserInputObject"] = json.dumps({
            "code": 200101,
            "data": {
                "content": [
                    {"type": "image", "data": url} for url in images
                ],
            },
        }, ensure_ascii=False)

    # 注册 task session
    async with _tasks_lock:
        _tasks[task_id] = TaskSession(task_id=task_id)
        # 初始化 SSE 队列
        _sse_subscribers[task_id] = []

    # 异步 POST 到 sidecar
    try:
        async with httpx.AsyncClient(timeout=30.0) as client:
            resp = await client.post(
                f"{SIDECAR_URL}{SIDECAR_CALLBACK_PATH}",
                json=accessctrl,
                headers={"Content-Type": "application/json"},
            )
            sidecar_status = resp.status_code
            sidecar_body = resp.text[:200]
    except Exception as e:
        logger.error("send_to_sidecar failed: %s", e)
        sidecar_status = 0
        sidecar_body = f"connection error: {e}"

    if sidecar_status not in (200, 202):
        # sidecar 请求失败——推 done 事件结束 SSE 流，不返回 task_id
        await _push_event(task_id, {
            "type": "error",
            "task_id": task_id,
            "message": f"sidecar 请求失败 (status={sidecar_status}): {sidecar_body[:120]}",
        })
        await _push_event(task_id, {"type": "done", "task_id": task_id})
        return JSONResponse(
            status_code=502,
            content={"error": f"sidecar 请求失败", "detail": sidecar_body[:200]},
        )

    # 记录发送结果
    await _push_event(task_id, {
        "type": "send_result",
        "task_id": task_id,
        "status_code": sidecar_status,
        "response_preview": sidecar_body,
        "agent_id": agent_id,
    })

    # 同步 dev 模式（httplib）：响应体可能直接含 TaskStatusReport / UserOutput，
    # 解析并推为事件，使 SSE 流能立即获取。
    # 异步模式（coreMind-mode / 有 SysAgentEntity）：sidecar 只返回 200 无 body
    # （sidecar_router.cpp 对齐 SysAgent 只 WriteStatueCode(200)），
    # 此时不推 done，由异步回调 handler 在收到回调后推 done。
    if resp.text.strip():
        try:
            sidecar_resp = json.loads(resp.text)
            data = sidecar_resp.get("Data", {})
            msg_type = data.get("MessageType", "")
            text = data.get("UserOutputText", "") or ""
            if msg_type == "TaskStatusReport" or msg_type == "UserOutput":
                # 同步模式——真正拿到了结果
                if text:
                    await _push_event(task_id, {
                        "type": "user_output",
                        "task_id": task_id,
                        "stream_status": "Completed",
                        "text": text,
                        "timestamp": datetime.now(timezone.utc).isoformat(),
                    })
                await _push_event(task_id, {
                    "type": "task_status",
                    "task_id": task_id,
                    "status_code": data.get("TaskStatusCode", 200),
                    "report": data.get("TaskStatusReport", "TaskCompleted"),
                    "timestamp": datetime.now(timezone.utc).isoformat(),
                })
                await _push_event(task_id, {"type": "done", "task_id": task_id})
            # 否则是异步模式——不推 done，等回调 handler 去推
        except Exception as parse_err:
            logger.warning("parse_sidecar_response failed: %s", parse_err)

    logger.info(
        "send task_id=%s agent=%s status=%d",
        task_id, agent_id, sidecar_status,
    )

    # 对齐生产行为：sidecar 异步模式只返回 200 无 body（sidecar_router.cpp
    # WriteStatueCode(200) 对齐 SysAgent），回调 Header.TaskID=原始 task_id，
    # 无需 request_id 映射——callback handler 直接用 TaskID 匹配 mock task_id。
    # （httplib dev 模式 main.cpp forward_to_bridge 同样已对齐为 200 无 body。）

    return JSONResponse({
        "task_id": task_id,
        "agent_id": agent_id,
        "sidecar_status": sidecar_status,
    })


@app.post("/coremind/systemagent/v1/callback")
async def handle_callback(request: Request) -> JSONResponse:
    """接收 sidecar 的异步回调，原始数据直接透传给前端 SSE，不做任何处理。"""
    body = await request.json()
    header = body.get("Header", {})
    data = body.get("Data", {})
    # 对齐生产行为：sidecar 回调 Header.TaskID 即原始 AccessCtrl TaskID（build_callback_header），
    # 直接作为 mock task_id 使用，无需 request_id 映射。
    mock_task_id = header.get("TaskID", "") or header.get("taskID", "")

    message_type = data.get("MessageType", "")

    logger.info(
        "callback task_id=%s msgType=%s",
        mock_task_id, message_type,
    )

    # 不解析字段，原样透传
    sse_event: dict[str, Any] = {
        "type": message_type,
        "task_id": mock_task_id,
        "data": data,
        "header": header,
        "raw": body,
    }
    await _push_event(mock_task_id, sse_event)

    # TaskStatusReport 额外推送 done 标记
    if message_type == "TaskStatusReport":
        await _push_event(mock_task_id, {"type": "done", "task_id": mock_task_id})

    return JSONResponse({"status": "ok"})


@app.get("/events/{task_id}")
async def stream_events(task_id: str):
    """SSE 流式返回指定 task 的回调事件。

    前端在发送请求后连接此端点，实时接收 UserOutput 块。
    """
    queue: asyncio.Queue = asyncio.Queue()

    async with _tasks_lock:
        session = _tasks.get(task_id)
        if session is None:
            return JSONResponse(
                {"error": f"task {task_id} not found"},
                status_code=404,
            )
        # 如果已有历史事件，先推历史
        for evt in session.events:
            await queue.put(evt)
        _sse_subscribers.setdefault(task_id, []).append(queue)

    async def event_stream() -> AsyncIterator[str]:
        try:
            while True:
                event = await queue.get()
                yield f"data: {json.dumps(event, ensure_ascii=False)}\n\n"
                if event.get("type") == "done":
                    break
        except asyncio.CancelledError:
            pass
        finally:
            async with _sse_lock:
                queues = _sse_subscribers.get(task_id, [])
                if queue in queues:
                    queues.remove(queue)

    return StreamingResponse(
        event_stream(),
        media_type="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",
            "Connection": "keep-alive",
        },
    )


@app.get("/health")
async def health():
    return {"status": "ok", "service": "coremind-upstream-mock", "port": MOCK_PORT}


@app.get("/tasks/{task_id}")
async def get_task(task_id: str):
    """查询 task 状态及已收到的回调事件。"""
    async with _tasks_lock:
        session = _tasks.get(task_id)
        if session is None:
            return JSONResponse({"error": "not found"}, status_code=404)
        return {
            "task_id": session.task_id,
            "dialogue_id": session.dialogue_id,
            "done": session.done,
            "event_count": len(session.events),
            "events": session.events,
        }


def main() -> None:
    logger.info("Starting CoreMind Upstream Mock on %s:%s", MOCK_HOST, MOCK_PORT)
    uvicorn.run(app, host=MOCK_HOST, port=MOCK_PORT, log_level="info")


if __name__ == "__main__":
    main()
