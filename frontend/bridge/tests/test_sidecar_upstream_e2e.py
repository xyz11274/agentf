#!/usr/bin/env python3
"""
上游→SideCar→Bridge 完整 E2E 测试。

模拟上游 CoreMind 平台发送 AccessCtrl 请求到 sidecar，
sidecar 处理后回调上游（或同步返回），验证全链路正确性。

测试拓扑:
  Mock Upstream (监听回调) ← SideCar (:23771) → Mock Bridge (:18399)
"""

import json
import os
import signal
import socket
import subprocess
import sys
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path
from threading import Thread

PROJECT_ROOT = Path(__file__).resolve().parents[3]
SIDECAR_BIN = PROJECT_ROOT / "sidecar" / "build-test" / "coremind_bridge"

# ── 合同 §1 完整输入样例 ───

CONTRACT_INPUT = {
    "Data": {
        "MessageType": "TaskAssignment",
        "UserInputText": "帮我推荐一下今天这件裤子适合怎样穿搭",
        # CMF 协议：UserInputObject 必须是 stringified JSON string（对齐 SysAgent
        # TaskAssignment::Parse 用 GetString）；传嵌套对象会被 sidecar 的 asString() 拒绝
        "UserInputObject": json.dumps({
            "code": 200101,
            "data": {
                "content": [
                    {"type": "image", "data": "data:image/png;base64,iVBORw0KGgoA..."}
                ]
            }
        }, ensure_ascii=False)
    },
    "Header": {
        "AcmSessionId": "",
        "ClientExField": "",
        "ClientSessionId": "session-DH_WD_LWWD_05_014-1783071771128030945",
        "DeviceCategory": "2",
        "DeviceId": "1.2.6d64bb1e63e24c5d_f7190b5edfd8af8f@sysagent.cu.gd.com.cn",
        "DialogueId": "",
        "EventTimeStamp": "2026-07-03T09:42:51Z",
        # B1 校验（is_supported_message_type）只接受 UserTaskManagement/TaskAssignment
        # 或 DataExchange/UserInput；合同 §1 样例的 DataExchange/TaskAssignment 会被 400 拒绝
        "MessageType": "UserTaskManagement",
        "Receiver": "SysAgent",
        "Sender": "AISFDEMO",
        "SeqNum": "39d17163be329ea4364e6f9c66b0d1ac",
        "SessionId": "",
        "SysAgentEntity": "",
        "TaskID": "04340661_0",
        "TeamEnvironmentID": "te_6d64bb1e63e24c5d_2_1783071771979",
        "UUID": "f7190b5edfd8af8f",
        "UserName": "6d64bb1e63e24c5d"
    }
}


# ── Mock Bridge ────────────────────────────────────────

class MockBridgeHandler(BaseHTTPRequestHandler):
    received: list[dict] = []

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("content-length", "0")))
        self.received.append({
            "path": self.path,
            "body": json.loads(body) if body else {},
            "headers": dict(self.headers),
        })
        resp = json.dumps({
            "task_id": "04340661_0",
            "content": "穿搭方案，参考如下",
            "artifacts": [
                {"type": "image", "url": "https://example.com/look.png",
                 "name": "look.png", "mime_type": "image/png"},
            ],
        }).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

    def log_message(self, *args):
        pass


# ── 辅助函数 ──────────────────────────────────────────

def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def start_server(handler):
    server = HTTPServer(("127.0.0.1", 0), handler)
    port = server.server_port
    t = Thread(target=server.serve_forever, daemon=True)
    t.start()
    return server, port


# ── 主测试 ────────────────────────────────────────────

def main():
    print("=== 上游→SideCar→Bridge 完整 E2E 测试 ===\n")

    if not SIDECAR_BIN.exists():
        print(f"[FAIL] SideCar binary not found: {SIDECAR_BIN}")
        sys.exit(1)

    # 1. 启动 Mock Bridge
    MockBridgeHandler.received = []
    bridge_server, bridge_port = start_server(MockBridgeHandler)
    print(f"[setup] Mock Bridge on :{bridge_port}")

    # 2. 启动 SideCar（httplib 模式，无回调）
    sidecar_port = free_port()
    proc = subprocess.Popen(
        [
            str(SIDECAR_BIN),
            "--listen", f"127.0.0.1:{sidecar_port}",
            "--bridge-url", f"http://127.0.0.1:{bridge_port}",
        ],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        env={**os.environ, "NO_PROXY": "127.0.0.1,localhost"},
    )
    print(f"[setup] SideCar on :{sidecar_port}")

    import httpx

    try:
        # 3. 等待 SideCar 就绪
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                print(f"[FAIL] SideCar exited: {proc.stdout.read()}")
                sys.exit(1)
            try:
                r = httpx.get(f"http://127.0.0.1:{sidecar_port}/health", timeout=0.3)
                if r.status_code == 200:
                    break
            except Exception:
                time.sleep(0.1)
        else:
            print("[FAIL] SideCar not healthy")
            sys.exit(1)

        # 4. 模拟上游：发送 AccessCtrl 请求
        print("\n── 测试 1：上游发送 AccessCtrl 请求 ──")
        resp = httpx.post(
            f"http://127.0.0.1:{sidecar_port}/coremind/systemagent/v1/callback",
            json=CONTRACT_INPUT,
            timeout=10,
        )
        print(f"   响应状态码: {resp.status_code}")

        body = resp.json()
        header = body.get("Header", {})
        data = body.get("Data", {})

        # 5. 验证 Bridge 收到正确格式
        print("\n── 测试 2：Bridge 收到标准化请求 ──")
        assert len(MockBridgeHandler.received) == 1, \
            f"Expected 1 Bridge request, got {len(MockBridgeHandler.received)}"
        bridge_req = MockBridgeHandler.received[0]["body"]

        assert bridge_req.get("user_id") == "6d64bb1e63e24c5d", \
            f"user_id: {bridge_req.get('user_id')}"
        assert bridge_req.get("client_id") == "session-DH_WD_LWWD_05_014-1783071771128030945", \
            f"client_id: {bridge_req.get('client_id')}"
        # 接口契约：content 必须是 ContentPart 数组（list[ContentPart]），不能是字符串、
        # 不能是嵌套 messages 结构（守护 §10 修复）
        content = bridge_req.get("content")
        assert isinstance(content, list), \
            f"content must be a ContentPart array, got {type(content).__name__}: {str(content)[:120]}"
        text_parts = [p["text"] for p in content if isinstance(p, dict) and p.get("type") == "text"]
        assert any("帮我推荐" in t for t in text_parts), \
            f"content 应包含 UserInputText, got text parts: {text_parts}"
        assert any(isinstance(p, dict) and p.get("type") == "image_url" for p in content), \
            f"content 应包含 image_url（图片转换后）, got: {content}"
        for p in content:
            assert "role" not in p and "messages" not in p, \
                f"content part must not nest message structure: {p}"
        print("   user_id    ✅")
        print("   client_id  ✅")
        print("   content    ✅ (ContentPart 数组，含文本+图片)")

        # 6. 验证响应符合 §3 TaskStatus 格式
        print("\n── 测试 3：响应符合 TaskStatus 格式 ──")
        assert header.get("MessageType") == "TaskStatusManagement", \
            f"Header.MessageType: {header.get('MessageType')}"
        assert header.get("Sender") == "SysAgent", \
            f"Sender should be original Receiver"
        # 任务结束信号（TaskStatusManagement）Receiver 固定为 SystemAgent
        assert header.get("Receiver") == "SystemAgent", \
            f"Receiver should be SystemAgent for task end signal"
        # SeqNum 透传上游请求 Header.SeqNum（上游发什么回什么）
        assert header.get("SeqNum") == CONTRACT_INPUT["Header"]["SeqNum"], \
            f"SeqNum should echo upstream request: {CONTRACT_INPUT['Header']['SeqNum']}, got {header.get('SeqNum')}"
        assert header.get("ClientSessionId") == CONTRACT_INPUT["Header"]["ClientSessionId"]
        assert header.get("TeamEnvironmentID") == CONTRACT_INPUT["Header"]["TeamEnvironmentID"]
        assert header.get("UUID") == CONTRACT_INPUT["Header"]["UUID"]
        assert header.get("DeviceId") == CONTRACT_INPUT["Header"]["DeviceId"]
        assert data.get("MessageType") == "TaskStatusReport", \
            f"Data.MessageType: {data.get('MessageType')}"
        assert data.get("TaskStatusCode") == 200, \
            f"TaskStatusCode: {data.get('TaskStatusCode')}"
        assert data.get("TaskStatusReport") == "TaskCompleted", \
            f"TaskStatusReport: {data.get('TaskStatusReport')}"
        assert "穿搭方案" in data.get("UserOutputText", ""), \
            "UserOutputText 应包含 Bridge 返回内容"
        print("   Header.MessageType    ✅")
        print("   Sender/Receiver 反转  ✅")
        print("   透传字段              ✅")
        print("   Data 字段             ✅")

        print("\n── 测试 4：SideCar 健康/能力/管理端点 ──")
        health = httpx.get(f"http://127.0.0.1:{sidecar_port}/health")
        assert health.status_code == 200
        assert health.json().get("coremind") is True
        print("   /health               ✅")

        ability = httpx.get(f"http://127.0.0.1:{sidecar_port}/sysagent/v1/scheduleTaskAbility")
        assert ability.status_code == 200
        assert ability.json().get("endpoint") == "/coremind/systemagent/v1/callback"
        print("   scheduleTaskAbility   ✅")

        install = httpx.post(f"http://127.0.0.1:{sidecar_port}/aisf/sysagent/v1/agentInstall")
        assert install.status_code == 200
        print("   agentInstall          ✅")

        # 7. 验证代理端点 stub 可用
        print("\n── 测试 5：平台代理端点 stub ──")
        for path in ("/proxy/memory/query", "/proxy/memory/store",
                     "/proxy/rag/search", "/proxy/context/add"):
            r = httpx.post(
                f"http://127.0.0.1:{sidecar_port}{path}",
                json={"test": True},
                timeout=3,
            )
            assert r.status_code == 200, f"{path} returned {r.status_code}"
            assert "stub" in r.json().get("status", ""), \
                f"{path}: expected stub, got {r.json()}"
        print("   4 个代理端点 stub     ✅")

        print("\n" + "=" * 50)
        print("[PASS] 所有 5 个测试通过！")

    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=3)
        bridge_server.shutdown()
        print("\n=== Test Complete ===")


if __name__ == "__main__":
    main()
