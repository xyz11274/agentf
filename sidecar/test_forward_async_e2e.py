#!/usr/bin/env python3
"""
端到端验证 sidecar forward_to_bridge（httplib dev 模式）异步回调链路对齐生产。

测试拓扑：
  AccessCtrl POST → sidecar (:23800) → mock Bridge (:23801) SSE 流
                                   └→ POST 回调 → mock callback server (:23802)

验证（对齐 sidecar_router.cpp 生产行为）：
  1. sidecar 异步模式返回 200 无 body
  2. EventType 校验：非法组合 400 拒绝
  3. UserInputObject 按 stringified JSON string 解析（多模态 content 透传 Bridge）
  4. 回调 Header.TaskID=原始 task_id、Sender/Receiver 翻转、透传字段
  5. 回调 Data.UserOutput 含 UserOutputObject/IsVoiceBroadcast/ImplicitOutputFlag/
     UserManualInterventionFlag
  6. TaskStatusReport 报文 TaskStatusCode=200/TaskCompleted

用法：
  python3 sidecar/test_forward_async_e2e.py
"""

import json
import signal
import subprocess
import sys
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from threading import Thread

SIDECAR_BIN = Path(__file__).parent / "build-dev" / "coremind_bridge"
SIDECAR_PORT = 23800
BRIDGE_PORT = 23801
CALLBACK_PORT = 23802
SIDECAR_URL = f"http://127.0.0.1:{SIDECAR_PORT}"

callbacks: list[dict] = []


# ── mock Bridge：/api/chat/{agent}?stream=true 返回 SSE ─────────────
class MockBridgeHandler(BaseHTTPRequestHandler):
    received: list[dict] = []

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("content-length", 0)))
        self.received.append({
            "path": self.path,
            "body": json.loads(body),
        })
        sse = "".join(
            f'data: {json.dumps(c, ensure_ascii=False)}\n\n' for c in [
                {"choices": [{"delta": {"content": "你"}}],
                 "_source": "coordinator", "_progress": True},
                {"choices": [{"delta": {"content": "好"}}],
                 "_source": "coordinator", "_progress": False},
                {"choices": [{"delta": {}}], "finish_reason": "stop",
                 "_source": "coordinator", "_progress": False,
                 "artifacts": [{"type": "image", "url": "http://x/look.png"}]},
            ]
        ) + "data: [DONE]\n\n"
        resp = sse.encode()
        self.send_response(200)
        self.send_header("content-type", "text/event-stream")
        self.send_header("content-length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

    def log_message(self, *args):
        pass


# ── mock callback server：接收 sidecar 回调 ────────────────────────
class MockCallbackHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("content-length", 0)))
        callbacks.append(json.loads(body))
        resp = b'{"status":"ok"}'
        self.send_response(200)
        self.send_header("content-length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

    def log_message(self, *args):
        pass


def _start_server(handler, port):
    server = HTTPServer(("127.0.0.1", port), handler)
    Thread(target=server.serve_forever, daemon=True).start()
    return server


def _http(method, url, body=None):
    req = urllib.request.Request(url, method=method)
    if body is not None:
        req.add_header("Content-Type", "application/json")
        req.data = json.dumps(body).encode()
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.status, resp.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def _accessctrl(message_type_pair, with_image=True, with_entity=True):
    header_mt, data_mt = message_type_pair
    hdr = {
        "MessageType": header_mt,
        "Sender": "UpstreamMock",
        "Receiver": "SysAgent",
        "UserName": "e2e-user",
        "DeviceId": "e2e-device",
        "DeviceCategory": "2",
        "UUID": "e2e-uuid",
        "ClientSessionId": "e2e-session",
        "TaskID": "e2e-task-001",
        "DialogueId": "e2e-dialogue",
        "AcmSessionId": "e2e-acm",
        "SessionId": "e2e-session-id",
        "SeqNum": "e2e-seq",
    }
    if with_entity:
        hdr["SysAgentEntity"] = f"127.0.0.1:{CALLBACK_PORT}"
    data = {
        "MessageType": data_mt,
        "UserInputText": "帮我推荐穿搭",
    }
    if with_image:
        # CMF 协议：stringified JSON string（非嵌套对象）
        data["UserInputObject"] = json.dumps({
            "code": 200101,
            "data": {"content": [{"type": "image", "data": "data:image/png;base64,abc"}]},
        }, ensure_ascii=False)
    return {"Header": hdr, "Data": data}


def _wait_callbacks(n, timeout=8.0):
    deadline = time.time() + timeout
    while time.time() < deadline and len(callbacks) < n:
        time.sleep(0.05)
    return len(callbacks)


def main() -> int:
    if not SIDECAR_BIN.exists():
        print(f"[FAIL] sidecar binary missing: {SIDECAR_BIN} (先 cmake --build build-dev)")
        return 1

    bridge_srv = _start_server(MockBridgeHandler, BRIDGE_PORT)
    cb_srv = _start_server(MockCallbackHandler, CALLBACK_PORT)

    sidecar = subprocess.Popen(
        [str(SIDECAR_BIN), "--listen", f"127.0.0.1:{SIDECAR_PORT}",
         "--bridge-url", f"http://127.0.0.1:{BRIDGE_PORT}"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )

    # 轮询 /health 直到 sidecar 就绪（避免启动竞态）
    ready = False
    for _ in range(40):
        if sidecar.poll() is not None:
            break
        try:
            status, _ = _http("GET", f"{SIDECAR_URL}/health")
            if status == 200:
                ready = True
                break
        except Exception:
            pass
        time.sleep(0.1)
    if not ready:
        print(f"[FAIL] sidecar 未就绪（pid={sidecar.pid}）")
        sidecar.kill()
        bridge_srv.shutdown()
        cb_srv.shutdown()
        return 1

    failures = []
    try:
        # ── 1. 非法消息类型 → 400 ──
        status, _ = _http("POST", f"{SIDECAR_URL}/coremind/systemagent/v1/callback",
                          _accessctrl(("DataExchange", "TaskAssignment")))
        if status != 400:
            failures.append(f"非法消息类型应 400，实际 {status}")

        # ── 2. 合法任务下发 + SysAgentEntity → 200 无 body + 异步回调 ──
        status, body = _http("POST", f"{SIDECAR_URL}/coremind/systemagent/v1/callback",
                             _accessctrl(("UserTaskManagement", "TaskAssignment")))
        if status != 200:
            failures.append(f"合法请求应 200，实际 {status}")
        if body.strip():
            failures.append(f"异步模式应 200 无 body，实际 body={body[:80]!r}")

        n = _wait_callbacks(3)
        if n < 3:
            failures.append(f"应收到至少 3 个回调（Running/Completed/TaskStatus），实际 {n}")

        # ── 3. Bridge 收到多模态 content（UserInputObject stringified 解析）──
        if not MockBridgeHandler.received:
            failures.append("Bridge 未收到转发请求")
        else:
            content = MockBridgeHandler.received[0]["body"].get("content", [])
            types = [p.get("type") for p in content]
            if "text" not in types or "image_url" not in types:
                failures.append(f"多模态 content 应含 text+image_url，实际 {types}")

        # ── 4. 回调字段校验 ──
        user_outputs = [c for c in callbacks if c.get("Data", {}).get("MessageType") == "UserOutput"]
        task_statuses = [c for c in callbacks if c.get("Data", {}).get("MessageType") == "TaskStatusReport"]
        if not user_outputs:
            failures.append("未收到 UserOutput 回调")
        else:
            header = user_outputs[0].get("Header", {})
            if header.get("TaskID") != "e2e-task-001":
                failures.append(f"回调 TaskID 应为原始 task_id，实际 {header.get('TaskID')!r}")
            if header.get("Sender") != "SysAgent" or header.get("Receiver") != "UpstreamMock":
                failures.append(f"Sender/Receiver 应翻转，实际 {header.get('Sender')}/{header.get('Receiver')}")
            if header.get("AcmSessionId") != "e2e-acm" or header.get("SessionId") != "e2e-session-id":
                failures.append("回调 Header 应透传 AcmSessionId/SessionId")
            data0 = user_outputs[0].get("Data", {})
            for field in ("UserOutputObject", "IsVoiceBroadcast", "ImplicitOutputFlag",
                          "UserManualInterventionFlag"):
                if field not in data0:
                    failures.append(f"UserOutput.Data 缺字段 {field}")
        completed = [c for c in user_outputs if c.get("Data", {}).get("StreamStatus") == "Completed"]
        if completed:
            obj = completed[0].get("Data", {}).get("UserOutputObject", "")
            if obj and "look.png" not in obj:
                failures.append(f"Completed UserOutputObject 应含 artifacts，实际 {obj[:80]!r}")
        if not task_statuses:
            failures.append("未收到 TaskStatusReport 回调")
        else:
            d = task_statuses[0].get("Data", {})
            if d.get("TaskStatusCode") != 200 or d.get("TaskStatusReport") != "TaskCompleted":
                failures.append(f"TaskStatus 应 200/TaskCompleted，实际 {d.get('TaskStatusCode')}/{d.get('TaskStatusReport')}")
            # 任务结束信号（TaskStatusManagement）Receiver 固定为 SystemAgent
            ts_header = task_statuses[0].get("Header", {})
            if ts_header.get("Receiver") != "SystemAgent":
                failures.append(f"TaskStatus Header.Receiver 应 SystemAgent，实际 {ts_header.get('Receiver')!r}")
            # SeqNum 透传上游请求 Header.SeqNum（上游发什么回什么）
            if ts_header.get("SeqNum") != "e2e-seq":
                failures.append(f"TaskStatus Header.SeqNum 应透传上游 e2e-seq，实际 {ts_header.get('SeqNum')!r}")

        # ── 5. 无 SysAgentEntity → 同步模式 200（Bridge 有响应）──
        MockBridgeHandler.received.clear()
        status, _ = _http("POST", f"{SIDECAR_URL}/coremind/systemagent/v1/callback",
                          _accessctrl(("UserTaskManagement", "TaskAssignment"), with_entity=False))
        if status != 200:
            failures.append(f"同步模式应 200，实际 {status}")
        if not MockBridgeHandler.received:
            failures.append("同步模式未转发到 Bridge")
        elif "?stream=false" not in MockBridgeHandler.received[0]["path"]:
            failures.append(f"同步模式应走 stream=false，实际 {MockBridgeHandler.received[0]['path']}")
    finally:
        sidecar.send_signal(signal.SIGTERM)
        try:
            sidecar.wait(timeout=3)
        except subprocess.TimeoutExpired:
            sidecar.kill()
        bridge_srv.shutdown()
        cb_srv.shutdown()

    if failures:
        print("[FAIL] forward_to_bridge e2e:")
        for f in failures:
            print(f"  - {f}")
        print(f"  callbacks={json.dumps(callbacks, ensure_ascii=False)[:500]}")
        return 1
    print(f"[PASS] forward_to_bridge 异步回调链路对齐生产 "
          f"（回调数={len(callbacks)}，Bridge 转发数={len(MockBridgeHandler.received)}）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
