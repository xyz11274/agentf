#!/usr/bin/env python3
"""
端到端验证 sidecar UDS LLM 转发链路。

测试拓扑：
  Python UDS 客户端 → sidecar UdsModelProxy → httplib → mock IMInferScheduler

验证：
  1. UDS 帧协议（4字节长度 + JSON）双向一致
  2. sidecar 接收 llm.chat 请求并转发到 HTTP 后端
  3. 响应通过 UDS 帧回传给客户端
"""

import json
import os
import signal
import socket
import struct
import subprocess
import sys
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from threading import Thread
from pathlib import Path

SIDECAR_BIN = Path(__file__).parent / "build-test" / "coremind_bridge"
UDS_SOCKET = "/tmp/test_sidecar_uds_e2e.sock"
MOCK_IM_INFER_PORT = 27322  # 避开默认 27321，防冲突
STUB_SERVICES = f"IMInferScheduler=127.0.0.1:{MOCK_IM_INFER_PORT}"

# ── mock IMInferScheduler ──────────────────────────────────────

class MockIMInferHandler(BaseHTTPRequestHandler):
    received_requests = []

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("content-length", 0)))
        self.received_requests.append({
            "path": self.path,
            "body": json.loads(body),
        })
        resp = json.dumps({
            "content": "Mock IMInferScheduler response OK",
            "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}
        }).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

    def log_message(self, *args):
        pass


def start_mock_im_infer():
    server = HTTPServer(("127.0.0.1", MOCK_IM_INFER_PORT), MockIMInferHandler)
    t = Thread(target=server.serve_forever, daemon=True)
    t.start()
    return server


# ── UDS 帧协议（与 sidecar / LlmUdsClient 一致）─────────────

def send_uds_frame(sock, data):
    payload = json.dumps(data, ensure_ascii=False).encode("utf-8")
    header = struct.pack(">I", len(payload))
    sock.sendall(header + payload)


def recv_uds_frame(sock):
    len_buf = sock.recv(4)
    if len(len_buf) < 4:
        return None
    frame_len = struct.unpack(">I", len_buf)[0]
    chunks = []
    remaining = frame_len
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            return None
        chunks.append(chunk)
        remaining -= len(chunk)
    return json.loads(b"".join(chunks))


# ── 测试主流程 ──────────────────────────────────────────────

def main():
    print("=== SideCar UDS LLM E2E Test ===\n")

    # 0. 检查 sidecar 二进制
    if not SIDECAR_BIN.exists():
        print(f"[FAIL] SideCar binary not found: {SIDECAR_BIN}")
        print("       Run: cmake -S sidecar -B sidecar/build-test -DCOREMIND_ENABLED=OFF && cmake --build sidecar/build-test")
        sys.exit(1)

    # 1. 启动 mock IMInferScheduler
    print(f"[setup] Starting mock IMInferScheduler on :{MOCK_IM_INFER_PORT}...")
    mock_server = start_mock_im_infer()
    time.sleep(0.2)

    # 2. 启动 sidecar（stub 模式 + UDS）
    sidecar_port = 23768  # 避开默认端口
    uds_path = UDS_SOCKET
    env = os.environ.copy()
    env["COREMIND_STUB_SERVICES"] = STUB_SERVICES
    env["AF_PLUGIN_UDS_FILE"] = uds_path

    proc = subprocess.Popen(
        [
            str(SIDECAR_BIN),
            "--listen", f"127.0.0.1:{sidecar_port}",
            "--bridge-url", "http://127.0.0.1:1",  # dummy, 测试不走 bridge
            "--uds-socket", uds_path,
            "--llm-api-url", f"http://127.0.0.1:{MOCK_IM_INFER_PORT}",
        ],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        env=env,
    )
    print(f"[setup] SideCar started on :{sidecar_port}, UDS={uds_path}")

    try:
        # 3. 等 sidecar 就绪
        import httpx
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                break
            try:
                r = httpx.get(f"http://127.0.0.1:{sidecar_port}/health", timeout=0.3)
                if r.status_code == 200:
                    break
            except Exception:
                time.sleep(0.1)
        else:
            print("[FAIL] SideCar did not become healthy")
            sys.exit(1)

        # 4. 发送 UDS llm.chat 请求
        print("\n[test] Connecting to UDS socket...")
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(5.0)
        sock.connect(uds_path)

        request = {
            "method": "llm.chat",
            "request_id": "e2e-test-001",
            "params": {
                "request": {
                    "model": "aisf/test-model",
                    "messages": [{"role": "user", "content": "Hello"}],
                    "stream": False,
                }
            }
        }
        print(f"[test] Sending: {json.dumps(request, ensure_ascii=False)[:120]}...")
        send_uds_frame(sock, request)

        # 5. 接收响应
        print("[test] Waiting for response...")
        response = recv_uds_frame(sock)
        if response is None:
            print("[FAIL] No response received (connection closed)")
            sys.exit(1)

        print(f"[test] Received: event_type={response.get('event_type')}, "
              f"done={response.get('done')}")
        print(f"       data={str(response.get('data', ''))[:200]}")

        # 6. 验证
        assert response.get("event_type") == "llm_final", \
            f"Expected llm_final, got {response.get('event_type')}"
        assert response.get("done") is True, "Expected done=true"
        print("\n[PASS] UDS LLM forwarding verified successfully!")

        # 7. 验证 mock IMInferScheduler 收到了请求
        assert len(MockIMInferHandler.received_requests) > 0, \
            "Mock IMInferScheduler received no requests"
        received = MockIMInferHandler.received_requests[0]
        print(f"\n[verify] Mock IMInferScheduler received: "
              f"path={received['path']}, model={received['body'].get('model', 'N/A')}")

    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=3)
        except Exception:
            proc.kill()
        mock_server.shutdown()
        if os.path.exists(uds_path):
            os.unlink(uds_path)
        if proc.stdout:
            print(f"\n[sidecar log]\n{proc.stdout.read()}"[-500:])
        print("\n=== Test Complete ===")


if __name__ == "__main__":
    main()
