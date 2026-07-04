"""
Bridge ⟷ 上游 SysAgent 适配层（候选实现，尚未接入 Bridge 运行时）

对标 HealthAgent 的 TaskRouter + AgentFlowManager + HaContext 机制。

工作方式:
  1. 接收上游 SysAgent 的 TASKASSIGNMENT 消息 (AccessCtrl 格式)
  2. 记录 HaContext (上游地址、taskID、用户信息)
  3. 异步执行 agentflowd 任务
  4. 完成后 POST 结果 (UserOutput / TaskStatusReport) 回上游回调 URL

支持两种模式:
  - async (默认): 收到请求 → 立即返回 200 → 后台执行 → POST 结果回上游
  - sync: 收到请求 → 等待执行完成 → HTTP 响应返回 (用于调试/测试)

用法:
  # 启动适配层
  当前不提供独立启动入口；完成 Bridge 契约评审后再接入。

  # 上游 SysAgent 下发任务 (async 模式)
  curl -X POST http://localhost:18080/coremind/systemagent/v1/callback \
    -H 'Content-Type: application/json' \
    -d '{
      "Header": {
        "MessageType": "UserTaskManagement",
        "TaskID": "ha_001",
        "Sender": "SystemAgent",
        "Receiver": "HealthAgent",
        "UserName": "user_001",
        "DeviceId": "dev_001",
        "SysAgentEntity": "10.0.0.1"
      },
      "Data": {
        "MessageType": "TaskAssignment",
        "UserInputText": "我今天穿什么出门"
      }
    }'
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import ssl
import time
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Callable

import httpx

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger("sysagent.aisf_adapter")

# ── 配置 ──────────────────────────────────────────────────────────

AISF_ADAPTER_HOST = os.environ.get("AISF_ADAPTER_HOST", "0.0.0.0")
AISF_ADAPTER_PORT = int(os.environ.get("AISF_ADAPTER_PORT", "18080"))
AISF_ADAPTER_SCHEME = os.environ.get("AISF_ADAPTER_SCHEME", "http")  # http / https

DAEMON_HOST = os.environ.get("DAEMON_HOST", "127.0.0.1")
DAEMON_PORT = int(os.environ.get("DAEMON_PORT", "17891"))

AGENT_ID = os.environ.get("AGENT_ID", "outfit-assistant")
CALLBACK_TIMEOUT = float(os.environ.get("CALLBACK_TIMEOUT", "360.0"))
SYNC_MODE = os.environ.get("SYNC_MODE", "0") == "1"  # 1=同步, 0=异步

# 上游 SysAgent 默认回调端口
UPSTREAM_CALLBACK_PORT = os.environ.get("UPSTREAM_CALLBACK_PORT", "23767")
UPSTREAM_CALLBACK_PATH = os.environ.get(
    "UPSTREAM_CALLBACK_PATH", "/coremind/systemagent/v1/callback"
)


# ═══════════════════════════════════════════════════════════════════
#  1. AccessCtrl 消息定义（仅实现需要的字段）
# ═══════════════════════════════════════════════════════════════════


@dataclass
class AccessCtrlHeader:
    """对标 HealthAgent MsgAisfHeader"""
    messageType: str = ""
    sender: str = ""
    receiver: str = ""
    userName: str = ""
    deviceId: str = ""
    deviceCategory: str = ""
    uuid: str = ""
    clientSessionId: str = ""
    acmSessionId: str = ""
    taskID: str = ""
    sysAgentEntity: str = ""
    teamEnvironmentID: str = ""
    eventTimeStamp: str = ""
    clientExField: str = ""


@dataclass
class TaskAssignmentData:
    """对标 HealthAgent TaskAssignment"""
    messageType: str = ""
    userInputText: str = ""
    userInputObject: str = ""
    userIntent: str = ""
    userSpecifiedAgent: str = ""


@dataclass
class UserOutputData:
    """对标 HealthAgent UserOutput"""
    messageType: str = ""
    userOutputText: str = ""
    userOutputObject: str = ""
    userManualInterventionFlag: bool = False
    implicitOutputFlag: bool = False
    streamStatus: str = ""


@dataclass
class TaskStatusData:
    """对标 HealthAgent TaskStatusManagement"""
    messageType: str = ""
    taskStatusCode: int = 0
    taskStatusReport: str = ""


def parse_accessctrl(body: dict) -> dict:
    """解析上游 SysAgent 的 AccessCtrl 消息，返回规范化字典。

    支持 PascalCase 字段名 (Header/Data/Payload)，这是上游 SysAgent 实际使用的格式。
    """
    header_raw = body.get("Header", {})
    data_raw = body.get("Data", {})
    payload_raw = body.get("Payload", {})

    # 完整字段提取
    parsed = {
        "header": {
            "messageType": header_raw.get("MessageType", ""),
            "sender": header_raw.get("Sender", ""),
            "receiver": header_raw.get("Receiver", ""),
            "userName": header_raw.get("UserName", ""),
            "deviceId": header_raw.get("DeviceId", ""),
            "deviceCategory": header_raw.get("DeviceCategory", ""),
            "uuid": header_raw.get("UUID", ""),
            "clientSessionId": header_raw.get("ClientSessionId", ""),
            "acmSessionId": header_raw.get("AcmSessionId", ""),
            "taskID": header_raw.get("TaskID", ""),
            "sysAgentEntity": header_raw.get("SysAgentEntity", ""),
            "teamEnvironmentID": header_raw.get("TeamEnvironmentID", ""),
            "eventTimeStamp": header_raw.get("EventTimeStamp", ""),
            "clientExField": header_raw.get("ClientExField", ""),
        },
        "data": {
            "messageType": data_raw.get("MessageType", ""),
            "userInputText": data_raw.get("UserInputText", ""),
            "userInputObject": data_raw.get("UserInputObject", ""),
            "userIntent": data_raw.get("UserIntent", ""),
            "userSpecifiedAgent": data_raw.get("UserSpecifiedAgent", ""),
        },
        "payload": {
            "function": payload_raw.get("Function", ""),
            "action": payload_raw.get("Action", ""),
        },
    }

    # 推断事件类型
    header_mt = parsed["header"]["messageType"]
    data_mt = parsed["data"]["messageType"]

    if header_mt == "UserTaskManagement" and data_mt == "TaskAssignment":
        parsed["eventType"] = "TASKASSIGNMENT"
    elif header_mt == "UserTaskManagement" and data_mt == "TaskStop":
        parsed["eventType"] = "TASKSTOP"
    elif header_mt == "DataExchange" and data_mt == "UserInput":
        parsed["eventType"] = "USERINPUT"
    elif header_mt == "DataExchange" and data_mt == "UserOutput":
        parsed["eventType"] = "USEROUTPUT"
    elif header_mt == "TaskStatusManagement":
        parsed["eventType"] = "TASKSTATUSMANAGEMENT"
    else:
        parsed["eventType"] = "UNKNOWN"

    return parsed


def extract_user_input(parsed: dict) -> str:
    """从解析后的消息中提取用户输入文本。"""
    return parsed.get("data", {}).get("userInputText", "")


# ═══════════════════════════════════════════════════════════════════
#  2. HaContext — 任务上下文生命周期（对标 HealthAgent）
# ═══════════════════════════════════════════════════════════════════


@dataclass
class HaContext:
    """对标 HealthAgent callBack.cpp 的 HaContext 结构体。

    每次收到上游 TASKASSIGNMENT 时创建，存放：
    - 上游回调地址 (sysAgentEntity)
    - 上游 taskID (parentTaskID)
    - AgentFlow 内部生成的 dialogueId / sessionId（首次为空，回调时更新）
    - 用户信息（用于构建回传报文）
    """
    sysAgentEntity: str = ""
    parentTaskID: str = ""
    sender: str = ""
    internalDialogueId: str = ""
    internalSessionId: str = ""
    userName: str = ""
    deviceId: str = ""
    deviceCategory: str = ""
    uuid: str = ""
    clientSessionId: str = ""
    acmSessionId: str = ""
    teamEnvironmentID: str = ""
    eventTimeStamp: str = ""
    clientExField: str = ""


class HaContextStore:
    """线程安全的 HaContext 存储。

    key = parentTaskID (上游的原始 TaskID)
    """

    def __init__(self):
        self._store: dict[str, HaContext] = {}
        # 额外按 (user_id, client_id) 记录最后使用的 dialogue_id
        # 因为上游每条消息的 taskID 不同，无法通过 taskID 找到上次的对话
        self._user_last_dialogue: dict[tuple[str, str], str] = {}
        self._lock = asyncio.Lock()

    async def save(self, parent_task_id: str, ctx: HaContext) -> None:
        async with self._lock:
            self._store[parent_task_id] = ctx
            logger.info(
                "HaContext saved: parentTaskID=%s sysAgentEntity=%s",
                parent_task_id, ctx.sysAgentEntity,
            )

    async def get(self, parent_task_id: str) -> HaContext | None:
        async with self._lock:
            return self._store.get(parent_task_id)

    async def get_last_dialogue_id(self, user_id: str, client_id: str) -> str:
        """获取同一用户上次使用的 dialogue_id（续旧对话用）。"""
        async with self._lock:
            return self._user_last_dialogue.get((user_id, client_id), "")

    async def set_last_dialogue_id(
        self, user_id: str, client_id: str, dialogue_id: str
    ) -> None:
        async with self._lock:
            if dialogue_id:
                self._user_last_dialogue[(user_id, client_id)] = dialogue_id

    async def update_dialogue_ids(
        self, task_id: str, dialogue_id: str, session_id: str
    ) -> None:
        """更新 HaContext 中的 AgentFlow 内部 ID（对标 updateHaContextInternalIds）。"""
        async with self._lock:
            # 向上逐层剥离 _N 后缀匹配（对标 HealthAgent 的 findHaContext 逻辑）
            key = task_id
            while key:
                if key in self._store:
                    ctx = self._store[key]
                    if dialogue_id:
                        ctx.internalDialogueId = dialogue_id
                    if session_id:
                        ctx.internalSessionId = session_id
                    logger.info(
                        "HaContext updated: taskID=%s → did=%s sid=%s",
                        key, dialogue_id, session_id,
                    )
                    return
                pos = key.rfind("_")
                if pos == -1:
                    break
                key = key[:pos]


ha_store = HaContextStore()


# ═══════════════════════════════════════════════════════════════════
#  3. 报文构建（对标 HealthAgent callBack.cpp 辅助函数）
# ═══════════════════════════════════════════════════════════════════


def make_header(ctx: HaContext, message_type: str) -> dict:
    """构建 AccessCtrl Header（对标 HealthAgent 的 header 构造逻辑）。

    协议字段定义见 docs/contracts/2.AISF-interface.md §2.1/§2.2。
    """
    # EventTimeStamp：优先复用请求时间戳，兜底当前时间
    ts = ctx.eventTimeStamp or datetime.now(timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ"
    )
    return {
        "MessageType": message_type,
        "Sender": "HealthAgent",
        "Receiver": ctx.sender,
        "UserName": ctx.userName,
        "DeviceId": ctx.deviceId,
        "DeviceCategory": ctx.deviceCategory,
        "UUID": ctx.uuid,
        "ClientSessionId": ctx.clientSessionId,
        "AcmSessionId": ctx.acmSessionId,
        "TaskID": ctx.parentTaskID,
        "EventTimeStamp": ts,
        "TeamEnvironmentID": ctx.teamEnvironmentID,
        "SysAgentEntity": ctx.sysAgentEntity,
        "ClientExField": ctx.clientExField,
    }


def build_user_output(ctx: HaContext, text: str, stream_status: str = "Completed") -> dict:
    """构建 UserOutput 报文（对标 BuildUserOutputMsg）。

    字段对齐 AISF 协议（docs/contracts/2.AISF-interface.md §2.1）：
      - IsVoiceBroadcast / UserOutputFlag 默认 0
      - UserOutputObject 留空（skill 返回的 code/data JSON，当前未消费）
    """
    return {
        "Header": make_header(ctx, "DataExchange"),
        "Data": {
            "MessageType": "UserOutput",
            "UserOutputText": text,
            "UserOutputObject": "",
            "StreamStatus": stream_status,
            "IsVoiceBroadcast": 0,
            "UserOutputFlag": 0,
        },
        "Payload": {"Function": "", "Action": ""},
    }


def build_task_status_report(ctx: HaContext, code: int, report: str) -> dict:
    """构建 TaskStatusReport 报文（对标 BuildTaskStatusReportMsg）。

    协议 §2.2 要求 Header.Receiver 固定为 "SystemAgent"，
    与 UserOutput（Receiver=请求 Sender）不同，需独立覆盖。
    """
    header = make_header(ctx, "TaskStatusManagement")
    header["Receiver"] = "SystemAgent"
    return {
        "Header": header,
        "Data": {
            "MessageType": "TaskStatusReport",
            "TaskStatusCode": code,
            "TaskStatusReport": report,
        },
        "Payload": {"Function": "", "Action": ""},
    }


# ═══════════════════════════════════════════════════════════════════
#  4. JSON-RPC 客户端（直连 agentflowd）
# ═══════════════════════════════════════════════════════════════════


class DaemonRpcClient:
    """JSON-RPC 2.0 over HTTP 客户端。"""

    def __init__(self, host: str = DAEMON_HOST, port: int = DAEMON_PORT):
        self._base_url = f"http://{host}:{port}"
        self._http = httpx.AsyncClient(timeout=CALLBACK_TIMEOUT + 10)
        self._counter = 0

    async def call(self, method: str, params: dict[str, Any] | None = None) -> Any:
        self._counter += 1
        payload = {
            "jsonrpc": "2.0",
            "method": method,
            "params": params or {},
            "id": self._counter,
        }
        logger.debug("→ daemon.%s %s", method, json.dumps(params, ensure_ascii=False)[:200])
        try:
            resp = await self._http.post(self._base_url, json=payload)
            resp.raise_for_status()
            body = resp.json()
        except httpx.TimeoutException:
            raise RuntimeError(f"daemon RPC timed out after {CALLBACK_TIMEOUT}s")
        except httpx.HTTPStatusError as e:
            raise RuntimeError(f"daemon HTTP {e.response.status_code}: {e.response.text[:500]}")
        except Exception as e:
            raise RuntimeError(f"daemon connection failed: {e}")

        if "error" in body and body["error"] is not None:
            err = body["error"]
            raise RuntimeError(f"daemon RPC error [{err.get('code')}]: {err.get('message')}")

        result = body.get("result")
        logger.debug("← daemon.%s → %s", method, json.dumps(result, ensure_ascii=False)[:200])
        return result

    async def close(self):
        await self._http.aclose()


async def execute_and_poll(
    daemon: DaemonRpcClient,
    user_input: str,
    user_id: str = "sysagent-user",
    client_id: str = "sysagent",
    dialogue_id: str = "",
    on_chunk: Callable[[str], None] | None = None,
) -> dict[str, Any]:
    """向 agentflowd 发送消息并轮询获取完整响应。

    Args:
        dialogue_id: 传入空字符串时自动生成；传入已有 ID 则复用（多轮重入）。

    返回 {"content": str, "task_id": str, "dialogue_id": str, "session_id": str}
    """
    if not dialogue_id:
        dialogue_id = (
            f"{datetime.now(timezone.utc).strftime('%m%d%H%M%S')}"
            f"-{uuid.uuid4().hex[:12]}"
        )

    # Step 1: agent.execute
    execute_result = await daemon.call("agent.execute", {
        "agent_id": AGENT_ID,
        "user_id": user_id,
        "client_id": client_id,
        "content": user_input,
        "dialogue_id": dialogue_id,
    })

    task_id = None
    if isinstance(execute_result, dict):
        task_id = execute_result.get("task_id") or execute_result.get("message_id")

    if not task_id:
        logger.warning("agent.execute returned no task_id, result=%s", execute_result)
        return {
            "content": str(execute_result or ""),
            "task_id": "",
            "dialogue_id": dialogue_id,
            "session_id": "",
        }

    logger.info("execute OK: task_id=%s dialogue_id=%s", task_id, dialogue_id)

    # Step 2: poll_response
    accumulated: list[str] = []
    deadline = time.monotonic() + CALLBACK_TIMEOUT
    poll_interval = 0.3

    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            logger.warning("poll timed out: task_id=%s", task_id)
            break

        try:
            poll_result = await daemon.call("agent.poll_response", {
                "agent_id": AGENT_ID,
                "session_id": "",
                "task_id": task_id,
                "timeout_ms": min(int(remaining * 1000), 10000),
            })
        except RuntimeError as e:
            logger.debug("poll error (retry): %s", e)
            await asyncio.sleep(poll_interval)
            continue

        if not poll_result:
            await asyncio.sleep(poll_interval)
            continue

        if isinstance(poll_result, dict):
            content = (
                poll_result.get("content")
                or poll_result.get("text")
                or poll_result.get("response")
                or ""
            )
            if content:
                accumulated.append(str(content))
                if on_chunk:
                    on_chunk(str(content))

            is_final = poll_result.get("final", False) or poll_result.get("is_done", False)
            if is_final:
                # 尝试从最终响应中提取 session_id
                session_id = poll_result.get("session_id", "")
                logger.info("task done: task_id=%s session_id=%s", task_id, session_id)
                break

        poll_interval = 0.3  # fast poll after first response

    full_content = "".join(accumulated)
    return {
        "content": full_content,
        "task_id": task_id,
        "dialogue_id": dialogue_id,
        "session_id": "",
    }


# ═══════════════════════════════════════════════════════════════════
#  5. 核心处理逻辑（对标 HealthAgent HandleAgentMsgRequest）
# ═══════════════════════════════════════════════════════════════════


async def handle_task_assignment(parsed: dict) -> dict:
    """处理 TASKASSIGNMENT 消息。

    对标 HealthAgent callBack.cpp:451-490 的 TASKASSIGNMENT 分支。
    """
    header = parsed["header"]
    data = parsed["data"]
    task_id = header.get("taskID", "")
    sender = header.get("sender", "")
    receiver = header.get("receiver", "")
    user_input = data.get("userInputText", "")
    sys_agent_entity = header.get("sysAgentEntity", "")

    logger.info(
        "TASKASSIGNMENT: taskID=%s sender=%s receiver=%s input=%s",
        task_id, sender, receiver, user_input[:80],
    )

    # 3a. GUI 桥接转发（receiver=GUIAgent）→ 暂不实现，记录日志
    if receiver == "GUIAgent":
        logger.warning("GUI bridge forwarding not yet implemented, taskID=%s", task_id)
        return {
            "status": "skipped",
            "reason": "GUI bridge not implemented",
            "task_id": task_id,
        }

    # 3b. 创建 HaContext（对标 HealthAgent 的 HaContext 保存逻辑）
    ctx = HaContext(
        sysAgentEntity=sys_agent_entity,
        parentTaskID=task_id,
        sender=sender,
        userName=header.get("userName", ""),
        deviceId=header.get("deviceId", ""),
        deviceCategory=header.get("deviceCategory", ""),
        uuid=header.get("uuid", ""),
        clientSessionId=header.get("clientSessionId", ""),
        acmSessionId=header.get("acmSessionId", ""),
        teamEnvironmentID=header.get("teamEnvironmentID", ""),
    )
    # 检查是否已有历史 dialogueId（续旧对话）
    # 上游每条消息 taskID 不同，所以按 (user_id, client_id) 查上次用的 dialogue_id
    prev_dialogue_id = await ha_store.get_last_dialogue_id(
        header.get("userName", ""),
        header.get("deviceId", ""),
    )
    if prev_dialogue_id:
        logger.info("reusing dialogue_id=%s for user=%s", prev_dialogue_id, header.get("userName", ""))

    await ha_store.save(task_id, ctx)

    # 执行 Agent
    daemon = DaemonRpcClient()
    try:
        result = await execute_and_poll(
            daemon,
            user_input,
            user_id=header.get("userName", "sysagent-user"),
            client_id=header.get("deviceId", "sysagent"),
            # 非首次：传入已有 dialogueId → daemon 路由到同一 dialogue 续旧对话
            # 首次：空 → execute_and_poll 自动生成
            dialogue_id=prev_dialogue_id,
        )

        # 更新 HaContext 中的内部 ID（对标 SysAgentMsgCallBack 的更新逻辑）
        await ha_store.update_dialogue_ids(
            task_id,
            result.get("dialogue_id", ""),
            result.get("session_id", ""),
        )
        # 同时记录这个用户最后用的 dialogue_id（供下次续旧对话）
        await ha_store.set_last_dialogue_id(
            header.get("userName", ""),
            header.get("deviceId", ""),
            result.get("dialogue_id", ""),
        )

        content = result.get("content", "")

        # 回传结果到上游（对标 ForwardToExternalSysAgent）
        if sys_agent_entity:
            await post_result_to_upstream(ctx, content)
            return {
                "status": "delivered",
                "task_id": task_id,
                "content_len": len(content),
            }
        else:
            # 没有上游地址，直接返回（相当于 sync 模式）
            return {
                "status": "completed",
                "task_id": task_id,
                "content": content,
            }

    except RuntimeError as e:
        logger.error("execution failed: taskID=%s error=%s", task_id, e)
        # 发送失败状态
        if sys_agent_entity:
            status_msg = build_task_status_report(ctx, 500, f"ExecutionFailed: {e}")
            await http_post_to_upstream(sys_agent_entity, status_msg)
        return {"status": "error", "task_id": task_id, "error": str(e)}
    finally:
        await daemon.close()


async def handle_non_task_assignment(parsed: dict) -> dict:
    """处理非 TASKASSIGNMENT 消息。

    对标 HealthAgent callBack.cpp:446-448:
      "非 TASKASSIGNMENT → AgentFlow 响应，转发到外部 SysAgent"
    """
    event_type = parsed.get("eventType", "UNKNOWN")
    task_id = parsed.get("header", {}).get("taskID", "")
    logger.info("non-TASKASSIGNMENT event: type=%s taskID=%s", event_type, task_id)

    # 查找 HaContext，找到则转发回上游
    ctx = await ha_store.get(task_id)
    if ctx and ctx.sysAgentEntity:
        data = parsed.get("data", {})
        text = data.get("userOutputText", "") or data.get("userInputText", "") or ""
        if text:
            await post_result_to_upstream(ctx, text)
        return {"status": "forwarded", "task_id": task_id}

    return {"status": "ignored", "reason": "no upstream target", "task_id": task_id}


async def post_result_to_upstream(ctx: HaContext, text: str) -> None:
    """将结果通过 UserOutput + TaskStatusReport 回传给上游 SysAgent。

    对标 HealthAgent:
      - BuildUserOutputMsg (带最终输出文本)
      - BuildTaskStatusReportMsg (taskStatusCode=200 表示完成)

    顺序：先发 UserOutput（结果正文），再发 TaskStatusReport（任务完成信号）。
    """
    if not ctx.sysAgentEntity:
        logger.warning("no sysAgentEntity, cannot post result back")
        return

    # 先发 UserOutput（最终结果）
    output_msg = build_user_output(ctx, text)
    await http_post_to_upstream(ctx.sysAgentEntity, output_msg)
    logger.info(
        "UserOutput sent: taskID=%s text_len=%d",
        ctx.parentTaskID, len(text),
    )

    # 再发 TaskStatusReport (code=200 = 完成)
    status_msg = build_task_status_report(ctx, 200, "TaskCompleted")
    await http_post_to_upstream(ctx.sysAgentEntity, status_msg)
    logger.info("TaskStatusReport sent: taskID=%s code=200", ctx.parentTaskID)


async def http_post_to_upstream(sys_agent_entity: str, body: dict) -> bool:
    """HTTP(S) POST 回传结果到上游 SysAgent（对标 HttpPostToSysAgent）。

    回调 URL 由 settings 决定：
      ``{scheme}://{sysAgentEntity}:{port}{path}``

    scheme 默认为 ``http``，设为 ``https`` 时自动启用 TLS。
    支持 mTLS 客户端证书（PEM 格式，可选密码）、自定义 CA 及跳过验证。
    """
    # 延迟导入避免与 routes/aisf_tasks.py 形成循环依赖
    from config import settings

    scheme = settings.aisf_adapter_upstream_scheme
    port   = settings.aisf_adapter_upstream_callback_port
    path   = settings.aisf_adapter_upstream_callback_path
    url    = f"{scheme}://{sys_agent_entity}:{port}{path}"
    payload = json.dumps(body, ensure_ascii=False)
    logger.debug("POST → %s (%d bytes)", url, len(payload))

    try:
        ssl_ctx = _build_upstream_ssl_context(settings)
        async with httpx.AsyncClient(timeout=10.0, verify=ssl_ctx) as client:
            resp = await client.post(
                url,
                content=payload,
                headers={"Content-Type": "text/json;charset=utf-8"},
            )
            resp.raise_for_status()
            logger.debug("POST OK: %s → %s", url, resp.status_code)
            return True
    except Exception as e:
        logger.error("POST failed: url=%s error=%s", url, e)
        return False


def _build_upstream_ssl_context(settings) -> ssl.SSLContext | bool:
    """根据配置构造上游回调的 SSLContext。

    返回 ``ssl.SSLContext``（推荐）或 ``True``/``False``（httpx 原生语义）。

    支持：
    - scheme=http → 不构建 SSLContext，返回 True（httpx 默认）
    - 自定义 CA 证书
    - 跳过服务端验证（verify=False）
    - mTLS 客户端证书（PEM，可选密码）
    """
    scheme = settings.aisf_adapter_upstream_scheme

    # HTTP → 无需 SSL
    if scheme != "https":
        return True

    has_client_cert = bool(settings.aisf_adapter_upstream_client_cert_path)
    has_ca_cert     = bool(settings.aisf_adapter_upstream_ca_cert_path)
    has_password    = bool(settings.aisf_adapter_upstream_client_key_password)
    skip_verify     = not settings.aisf_adapter_upstream_verify

    # 没有任何定制 → 使用系统默认 CA（True）
    if not has_client_cert and not has_ca_cert and not skip_verify:
        return True

    ctx = ssl.create_default_context()

    # 1) 服务端证书验证
    if skip_verify:
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
    elif has_ca_cert:
        ctx.load_verify_locations(settings.aisf_adapter_upstream_ca_cert_path)
    # else: 使用系统默认 CA

    # 2) 客户端证书（mTLS）
    if has_client_cert:
        key_path = settings.aisf_adapter_upstream_client_key_path or None
        if has_password:
            ctx.load_cert_chain(
                settings.aisf_adapter_upstream_client_cert_path,
                key_path,
                password=settings.aisf_adapter_upstream_client_key_password,
            )
        else:
            ctx.load_cert_chain(
                settings.aisf_adapter_upstream_client_cert_path,
                key_path,
            )

    return ctx


# ═══════════════════════════════════════════════════════════════════
#  6. HTTP 处理入口（对标 HealthAgent HandleUserRequest）
# ═══════════════════════════════════════════════════════════════════


async def handle_callback(request_body: bytes) -> tuple[int, dict]:
    """处理上游 SysAgent 的回调请求。

    对标 HealthAgent HandleUserRequest + HandleAgentMsgRequest。
    """
    try:
        raw_body = json.loads(request_body)
    except json.JSONDecodeError as e:
        return 400, {"error": f"invalid JSON: {e}"}

    # 解析 AccessCtrl
    parsed = parse_accessctrl(raw_body)
    event_type = parsed.get("eventType", "UNKNOWN")
    task_id = parsed.get("header", {}).get("taskID", "")

    logger.info(
        "→ callback: eventType=%s taskID=%s input=%s",
        event_type,
        task_id,
        extract_user_input(parsed)[:80],
    )

    # ── 路由逻辑（对标 HealthAgent HandleAgentMsgRequest） ──

    # 1. GUI 回调: sender=GUIAgent, 非 TASKASSIGNMENT → 路由回 coordinator
    sender = parsed.get("header", {}).get("sender", "")
    if sender == "GUIAgent" and event_type != "TASKASSIGNMENT":
        logger.info("GUI callback routing (return to coordinator), taskID=%s", task_id)
        # 查找 HaContext，用已有 dialogueId/sessionId 直达 coordinator
        ctx = await ha_store.get(task_id)
        if ctx and ctx.internalDialogueId:
            # 通过 agent.execute + 已有 dialogueId 实现重入
            daemon = DaemonRpcClient()
            try:
                result = await execute_and_poll(
                    daemon,
                    extract_user_input(parsed),
                    dialogue_id=ctx.internalDialogueId,
                )
                await ha_store.update_dialogue_ids(
                    task_id,
                    result.get("dialogue_id", ""),
                    result.get("session_id", ""),
                )
                if ctx.sysAgentEntity:
                    await post_result_to_upstream(ctx, result.get("content", ""))
            finally:
                await daemon.close()
            return 200, {"status": "gui_callback_processed", "task_id": task_id}
        return 200, {"status": "gui_callback_no_context", "task_id": task_id}

    # 2. 非 TASKASSIGNMENT → AgentFlow 响应，转发到外部 SysAgent
    if event_type != "TASKASSIGNMENT":
        result = await handle_non_task_assignment(parsed)
        return 200, result

    # 3. TASKASSIGNMENT → 按 receiver 分发
    if event_type == "TASKASSIGNMENT":
        receiver = parsed.get("header", {}).get("receiver", "")
        # 3a. GUIAgent → 桥接转发
        if receiver == "GUIAgent":
            logger.info("GUI bridge forward: taskID=%s (not implemented)", task_id)
            return 200, {"status": "gui_bridge_skipped", "task_id": task_id}
        # 3b. 本 Agent → 执行
        if SYNC_MODE:
            # 同步模式：等结果再返回
            result = await handle_task_assignment(parsed)
            return 200, result
        else:
            # 异步模式：后台执行，立即返回 200
            asyncio.create_task(handle_task_assignment(parsed))
            return 200, {
                "status": "accepted",
                "task_id": task_id,
                "mode": "async",
            }

    return 400, {"error": f"unknown event type: {event_type}"}


# ═══════════════════════════════════════════════════════════════════
#  7. ASGI 应用（对标 HealthAgent TaskRouter 路由表）
# ═══════════════════════════════════════════════════════════════════


async def app(scope: dict, receive: Callable, send: Callable) -> None:
    if scope["type"] != "http":
        return

    method = scope["method"]
    path = scope["path"]

    # ── 健康检查（对标 HealthAgent /healthcheck） ──
    if path == "/healthcheck" and method == "GET":
        return await _send_json(send, 200, {
            "status": "healthy",
            "agent": AGENT_ID,
            "mode": "async" if not SYNC_MODE else "sync",
            "ha_context_count": len(ha_store._store),
        })

    # ── 能力宣告（对标 HealthAgent /sysagent/v1/scheduleTaskAbility） ──
    if path == "/sysagent/v1/scheduleTaskAbility" and method in ("GET", "POST"):
        return await _send_json(send, 200, {
            "agent_name": AGENT_ID,
            "version": "1.0.0",
            "capabilities": [
                "outfit_consult",
                "weather_query",
                "occasion_guide",
                "image_generation",
                "video_generation",
            ],
            "endpoint": (
                f"{AISF_ADAPTER_SCHEME}://{AISF_ADAPTER_HOST}:{AISF_ADAPTER_PORT}"
                f"/coremind/systemagent/v1/callback"
            ),
            "async_mode": not SYNC_MODE,
        })

    # ── Agent 安装/更新/卸载接口（对标 HealthAgent agentInstall / agentUpdate / agentUninstall） ──
    if path == "/aisf/sysagent/v1/agentInstall" and method == "POST":
        return await _send_json(send, 200, {"status": "ok", "action": "install"})
    if path == "/aisf/sysagent/v1/agentUpdate" and method == "POST":
        return await _send_json(send, 200, {"status": "ok", "action": "update"})
    if path == "/aisf/sysagent/v1/agentUninstall" and method == "POST":
        return await _send_json(send, 200, {"status": "ok", "action": "uninstall"})

    # ── 任务入口（对标 HealthAgent /coremind/systemagent/v1/callback） ──
    if path in (
        "/coremind/systemagent/v1/callback",
        "/aisf/outfit/v1/tasks",
        "/health/v1/tasks",
    ) and method == "POST":
        body_bytes = b""
        while True:
            event = await receive()
            if event["type"] == "http.request":
                body_bytes += event.get("body", b"")
                if not event.get("more_body", False):
                    break
            elif event["type"] == "http.disconnect":
                return

        status, data = await handle_callback(body_bytes)
        return await _send_json(send, status, data)

    # ── 404 ──
    await _send_json(send, 404, {"error": "not found", "path": path})


async def _send_json(send: Callable, status: int, data: dict) -> None:
    body = json.dumps(data, ensure_ascii=False).encode("utf-8")
    await send({
        "type": "http.response.start",
        "status": status,
        "headers": [
            (b"content-type", b"application/json; charset=utf-8"),
            (b"content-length", str(len(body)).encode()),
        ],
    })
    await send({"type": "http.response.body", "body": body})


def main():
    import uvicorn

    logger.info(
        "SysAgent aisf_adapter starting: %s:%s → daemon %s:%s agent=%s mode=%s",
        AISF_ADAPTER_HOST, AISF_ADAPTER_PORT,
        DAEMON_HOST, DAEMON_PORT,
        AGENT_ID,
        "sync" if SYNC_MODE else "async",
    )
    uvicorn.run(app, host=AISF_ADAPTER_HOST, port=AISF_ADAPTER_PORT, log_level="info")


if __name__ == "__main__":
    main()
