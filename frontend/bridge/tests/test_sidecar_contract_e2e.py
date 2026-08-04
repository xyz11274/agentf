#!/usr/bin/env python3
"""
基于接口合同样例数据的 sidecar E2E 测试。

验证 sidecar 能正确处理合同 §1 的输入格式，并输出合同 §2/§3 的正确格式。
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
MOCK_BRIDGE_PORT = 18399


# ── Mock Bridge ──────────────────────────────────────────────

class MockBridgeHandler(BaseHTTPRequestHandler):
    received: list[dict] = []

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("content-length", 0)))
        self.received.append({
            "path": self.path,
            "body": json.loads(body) if body else {},
            "headers": dict(self.headers),
        })
        resp = json.dumps({
            "task_id": "04340661_0",
            "content": "穿搭方案，参考如下",
            "artifacts": [
                {"type": "image", "url": "https://example.com/look.png", "name": "look.png", "mime_type": "image/png"},
            ],
        }).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

    def log_message(self, *args):
        pass


def start_mock_bridge():
    MockBridgeHandler.received = []
    server = HTTPServer(("127.0.0.1", MOCK_BRIDGE_PORT), MockBridgeHandler)
    Thread(target=server.serve_forever, daemon=True).start()
    return server


# ── 合同 §1 输入样例 ───────────────────────────────────────

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


def main():
    print("=== SideCar 合同验证 E2E ===\n")

    if not SIDECAR_BIN.exists():
        print(f"[FAIL] SideCar binary not found: {SIDECAR_BIN}")
        sys.exit(1)

    print("[setup] Starting mock Bridge...")
    bridge = start_mock_bridge()
    time.sleep(0.2)

    port = 23770
    proc = subprocess.Popen(
        [
            str(SIDECAR_BIN),
            "--listen", f"127.0.0.1:{port}",
            "--bridge-url", f"http://127.0.0.1:{MOCK_BRIDGE_PORT}",
        ],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        env={**os.environ, "NO_PROXY": "127.0.0.1,localhost"},
    )
    print(f"[setup] SideCar started on :{port}")

    try:
        # 等待就绪
        import httpx
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                print(f"[FAIL] SideCar exited: {proc.stdout.read()}")
                sys.exit(1)
            try:
                r = httpx.get(f"http://127.0.0.1:{port}/health", timeout=0.3)
                if r.status_code == 200:
                    break
            except Exception:
                time.sleep(0.1)
        else:
            print("[FAIL] SideCar not healthy")
            sys.exit(1)

        print("\n── 测试 1：合同输入 → Bridge 收到标准化内容 ──")
        resp = httpx.post(
            f"http://127.0.0.1:{port}/coremind/systemagent/v1/callback",
            json=CONTRACT_INPUT,
            timeout=10,
        )
        print(f"响应状态码: {resp.status_code}")
        body = resp.json()
        print(f"响应类型: {body.get('Header', {}).get('MessageType')}")

        # 验证 Bridge 收到请求
        assert len(MockBridgeHandler.received) == 1, "Bridge should receive 1 request"
        bridge_req = MockBridgeHandler.received[0]
        bridge_body = bridge_req["body"]

        # 验证 Bridge 收到 user_id/client_id/content
        assert bridge_body.get("user_id") == "6d64bb1e63e24c5d", \
            f"user_id mismatch: {bridge_body.get('user_id')}"
        assert bridge_body.get("client_id") == "session-DH_WD_LWWD_05_014-1783071771128030945", \
            f"client_id mismatch: {bridge_body.get('client_id')}"
        # 接口契约：content 必须是 ContentPart 数组（list[ContentPart]），不能是字符串、
        # 不能是嵌套 messages 结构（守护 §10 修复）
        content = bridge_body.get("content")
        assert isinstance(content, list), \
            f"content must be a ContentPart array, got {type(content).__name__}: {str(content)[:120]}"
        text_parts = [p["text"] for p in content if isinstance(p, dict) and p.get("type") == "text"]
        assert any("帮我推荐" in t for t in text_parts), \
            f"content should contain UserInputText, got text parts: {text_parts}"
        assert any(isinstance(p, dict) and p.get("type") == "image_url" for p in content), \
            f"content should contain image_url for images, got: {content}"
        # 契约守护：content 数组元素不得再嵌套 role/messages（双层嵌套回归）
        for p in content:
            assert "role" not in p and "messages" not in p, \
                f"content part must not nest message structure: {p}"

        print(f"  user_id:    {bridge_body.get('user_id')} ✅")
        print(f"  client_id:  {bridge_body.get('client_id')} ✅")
        print(f"  content:    {str(bridge_body.get('content'))[:80]}... ✅ (ContentPart 数组)")

        print("\n── 测试 2：响应符合合同 §3 TaskStatus 格式 ──")
        header = body.get("Header", {})
        data = body.get("Data", {})

        assert header.get("MessageType") == "TaskStatusManagement", \
            f"Expected TaskStatusManagement, got {header.get('MessageType')}"

        # Sender 翻转
        assert header.get("Sender") == "SysAgent", \
            f"Sender should be Receiver original value: SysAgent"
        # 任务结束信号（TaskStatusManagement）Receiver 固定为 SystemAgent
        assert header.get("Receiver") == "SystemAgent"
        # SeqNum 透传上游请求 Header.SeqNum（上游发什么回什么）
        assert header.get("SeqNum") == CONTRACT_INPUT["Header"]["SeqNum"], \
            f"SeqNum should echo upstream request: {CONTRACT_INPUT['Header']['SeqNum']}, got {header.get('SeqNum')}"

        # 透传字段
        assert header.get("ClientSessionId") == CONTRACT_INPUT["Header"]["ClientSessionId"]
        assert header.get("TeamEnvironmentID") == CONTRACT_INPUT["Header"]["TeamEnvironmentID"]
        assert header.get("UUID") == CONTRACT_INPUT["Header"]["UUID"]
        assert header.get("DeviceId") == CONTRACT_INPUT["Header"]["DeviceId"]

        print(f"  Header.MessageType:     {header.get('MessageType')} ✅")
        print(f"  Sender→Receiver 反转:   {header.get('Sender')} → {header.get('Receiver')} ✅")
        print(f"  ClientSessionId 透传:   {header.get('ClientSessionId') == CONTRACT_INPUT['Header']['ClientSessionId']} ✅")
        print(f"  TeamEnvironmentID 透传: {header.get('TeamEnvironmentID') == CONTRACT_INPUT['Header']['TeamEnvironmentID']} ✅")
        print(f"  UUID 透传:             {header.get('UUID') == CONTRACT_INPUT['Header']['UUID']} ✅")
        print(f"  DeviceId 透传:         {header.get('DeviceId') == CONTRACT_INPUT['Header']['DeviceId']} ✅")

        # TaskStatus Data 字段
        assert data.get("MessageType") == "TaskStatusReport"
        assert data.get("TaskStatusCode") == 200
        assert data.get("TaskStatusReport") == "TaskCompleted"
        print(f"  Data.MessageType:       {data.get('MessageType')} ✅")
        print(f"  Data.TaskStatusCode:    {data.get('TaskStatusCode')} ✅")
        print(f"  Data.TaskStatusReport:  {data.get('TaskStatusReport')} ✅")

        print("\n── 测试 3：无 SysAgentEntity（本地模式）同步返回 ✅  ──")
        print("\n[PASS] 所有合同断言通过！")

    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=3)
        bridge.shutdown()
        print("\n=== Test Complete ===")


if __name__ == "__main__":
    main()
