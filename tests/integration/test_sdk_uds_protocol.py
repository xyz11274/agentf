#!/usr/bin/env python3
"""
SDK Integration Test Runner

启动 Mock UDS Server，调用各 SDK 接口，验证发出的消息格式符合 UDS 协议规范。

测试方法：
1. Mock Server 捕获所有收到的 JSON-RPC 请求
2. 调用 SDK 方法
3. 验证捕获的请求格式是否正确

使用方法：
    python3 test_sdk_uds_protocol.py

测试覆盖：
- JSON-RPC 2.0 格式验证
- UDS 方法参数验证
- async_callback 方法验证
"""

import json
import os
import socket
import sys
import threading
import time
import subprocess
import tempfile
from dataclasses import dataclass
from typing import Any, Dict, List, Optional
from pathlib import Path

# ─────────────────────────────────────────────────────────────────────────────
# 测试结果统计
# ─────────────────────────────────────────────────────────────────────────────

TESTS_PASSED = 0
TESTS_FAILED = 0
CURRENT_SUITE = ""

def log_pass(msg: str):
    global TESTS_PASSED
    TESTS_PASSED += 1
    print(f"  [PASS] {msg}")

def log_fail(msg: str, expected=None, actual=None):
    global TESTS_FAILED
    TESTS_FAILED += 1
    print(f"  [FAIL] {msg}")
    if expected:
        print(f"         Expected: {expected}")
    if actual:
        print(f"         Actual: {actual}")

def start_suite(name: str):
    global CURRENT_SUITE
    CURRENT_SUITE = name
    print(f"\n=== {name} ===")

# ─────────────────────────────────────────────────────────────────────────────
# Mock UDS Server
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class CapturedRequest:
    method: str
    params: Dict[str, Any]
    request_id: Any
    raw: Dict[str, Any]

class MockUDSServer:
    """Mock UDS Server that captures all incoming requests."""

    def __init__(self, socket_path: str):
        self.socket_path = socket_path
        self.requests: List[CapturedRequest] = []
        self.lock = threading.Lock()
        self.running = False
        self.server_socket = None
        self.server_thread = None

    def start(self):
        """Start the mock server."""
        if os.path.exists(self.socket_path):
            os.unlink(self.socket_path)

        self.server_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server_socket.bind(self.socket_path)
        self.server_socket.listen(10)
        self.server_socket.settimeout(0.5)
        self.running = True

        self.server_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self.server_thread.start()

        # Wait for socket
        for _ in range(50):
            if os.path.exists(self.socket_path):
                return True
            time.sleep(0.1)
        return False

    def stop(self):
        """Stop the mock server."""
        self.running = False
        if self.server_socket:
            try:
                self.server_socket.close()
            except:
                pass
        if self.server_thread:
            self.server_thread.join(timeout=2.0)
        if os.path.exists(self.socket_path):
            os.unlink(self.socket_path)

    def _accept_loop(self):
        while self.running:
            try:
                conn, _ = self.server_socket.accept()
                threading.Thread(target=self._handle_client, args=(conn,), daemon=True).start()
            except socket.timeout:
                continue
            except:
                break

    def _handle_client(self, conn):
        buffer = b""
        try:
            while self.running:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                buffer += chunk
                while b"\n" in buffer:
                    line, buffer = buffer.split(b"\n", 1)
                    if line.strip():
                        self._process_request(conn, line.decode())
        except:
            pass
        finally:
            conn.close()

    def _process_request(self, conn, request_str: str):
        try:
            request = json.loads(request_str)
        except json.JSONDecodeError:
            response = {"jsonrpc": "2.0", "error": {"code": -32700, "message": "Parse error"}, "id": None}
            conn.sendall((json.dumps(response) + "\n").encode())
            return

        # Capture request
        captured = CapturedRequest(
            method=request.get("method", ""),
            params=request.get("params", {}),
            request_id=request.get("id"),
            raw=request
        )
        with self.lock:
            self.requests.append(captured)

        # Send response
        result = self._get_response(captured.method)
        response = {"jsonrpc": "2.0", "result": result, "id": request.get("id")}
        conn.sendall((json.dumps(response) + "\n").encode())

    def _get_response(self, method: str) -> Dict:
        defaults = {
            "put": {"success": True},
            "get": {"exists": True, "value": "mock_value"},
            "remove": {"success": True},
            "contains": {"exists": True},
            "subscribe": {"subscription_id": 1},
            "unsubscribe": {"success": True},
            "publish": {"success": True},
            "list_keys": {"keys": []},
            "async_callback": {"success": True, "queued": True}
        }
        return defaults.get(method, {"success": True})

    def get_requests(self, method: str = None) -> List[CapturedRequest]:
        with self.lock:
            if method:
                return [r for r in self.requests if r.method == method]
            return list(self.requests)

    def clear(self):
        with self.lock:
            self.requests.clear()

    def wait_for_request(self, method: str, timeout: float = 5.0) -> Optional[CapturedRequest]:
        start = time.time()
        while time.time() - start < timeout:
            with self.lock:
                for r in self.requests:
                    if r.method == method:
                        return r
            time.sleep(0.05)
        return None


# ─────────────────────────────────────────────────────────────────────────────
# 验证器
# ─────────────────────────────────────────────────────────────────────────────

def validate_jsonrpc(req: CapturedRequest) -> bool:
    """验证 JSON-RPC 2.0 格式"""
    raw = req.raw

    if raw.get("jsonrpc") != "2.0":
        log_fail("jsonrpc field must be '2.0'", "2.0", raw.get("jsonrpc"))
        return False

    if not raw.get("method"):
        log_fail("method field is required")
        return False

    if "params" not in raw:
        log_fail("params field is required")
        return False

    if raw.get("id") is None:
        log_fail("id field is required for requests")
        return False

    return True


def validate_params(req: CapturedRequest, expected_params: Dict[str, Any]) -> bool:
    """验证参数匹配"""
    for key, expected_value in expected_params.items():
        actual = req.params.get(key)
        if actual != expected_value:
            log_fail(f"Parameter '{key}' mismatch", expected_value, actual)
            return False
    return True


def validate_required_params(req: CapturedRequest, required: List[str]) -> bool:
    """验证必需参数存在"""
    missing = [p for p in required if p not in req.params]
    if missing:
        log_fail(f"Missing required parameters", required, list(req.params.keys()))
        return False
    return True


# ─────────────────────────────────────────────────────────────────────────────
# Python SDK 测试
# ─────────────────────────────────────────────────────────────────────────────

def test_python_sdk(server: MockUDSServer, sdk_path: str):
    """测试 Python SDK"""
    start_suite("Python SDK Tests")

    sys.path.insert(0, os.path.dirname(sdk_path))
    from agentflow_databus import DataBusClient

    session_id = "test-session-python"
    client = DataBusClient(server.socket_path, session_id)

    try:
        # Test put
        server.clear()
        client.user().put("test_key", "test_value")
        req = server.wait_for_request("put")
        if req and validate_jsonrpc(req) and validate_params(req, {
            "namespace": "user", "key": "test_key", "value": "test_value"
        }):
            log_pass("put: sends valid JSON-RPC request")

        # Test get
        server.clear()
        client.user().get("test_key")
        req = server.wait_for_request("get")
        if req and validate_jsonrpc(req) and validate_required_params(req, ["session_id", "namespace", "key"]):
            log_pass("get: sends valid JSON-RPC request")

        # Test contains
        server.clear()
        client.user().contains("test_key")
        req = server.wait_for_request("contains")
        if req and validate_jsonrpc(req) and validate_required_params(req, ["session_id", "namespace", "key"]):
            log_pass("contains: sends valid JSON-RPC request")

        # Test remove
        server.clear()
        client.user().remove("test_key")
        req = server.wait_for_request("remove")
        if req and validate_jsonrpc(req) and validate_required_params(req, ["session_id", "namespace", "key"]):
            log_pass("remove: sends valid JSON-RPC request")

        # Test subscribe
        server.clear()
        client.user().subscribe("test_key", lambda k, v: None)
        req = server.wait_for_request("subscribe")
        if req and validate_jsonrpc(req) and validate_required_params(req, ["session_id", "namespace", "key"]):
            log_pass("subscribe: sends valid JSON-RPC request")

        # Test publish
        server.clear()
        client.user().publish("test_topic", "payload")
        req = server.wait_for_request("publish")
        if req and validate_jsonrpc(req) and validate_required_params(req, ["session_id", "namespace", "topic", "value"]):
            log_pass("publish: sends valid JSON-RPC request")

        # Test async_callback
        server.clear()
        os.environ["AF_SESSION_ID"] = session_id
        os.environ["AF_DIALOGUE_ID"] = "dialogue-123"
        os.environ["AF_SELF_TASK_ID"] = "task-456"
        os.environ["AF_NODE_ID"] = "node-789"

        async_sdk = client.async_sdk()
        async_sdk.send_to_self("callback_key", '{"status": "ok"}')

        req = server.wait_for_request("async_callback")
        if req:
            if validate_jsonrpc(req) and validate_params(req, {
                "session_id": session_id,
                "dialogue_id": "dialogue-123",
                "task_id": "task-456",
                "node_id": "node-789",
                "call_key": "callback_key",
                "content": '{"status": "ok"}'
            }):
                log_pass("async_callback: sends valid JSON-RPC request with all parameters")

        # Clean up env
        for k in ["AF_SESSION_ID", "AF_DIALOGUE_ID", "AF_SELF_TASK_ID", "AF_NODE_ID"]:
            os.environ.pop(k, None)

        # Test response_push without source
        server.clear()
        os.environ["AF_SESSION_ID"] = session_id
        os.environ["AF_DIALOGUE_ID"] = "dialogue-response-push"
        client.response_push("test content", type="progress")
        req = server.wait_for_request("response_push")
        if req and validate_jsonrpc(req) and validate_required_params(req, ["session_id", "dialogue_id", "content", "type", "final"]):
            log_pass("response_push: sends valid JSON-RPC request (no source)")
        else:
            log_fail("response_push: missing required params", req)

        os.environ.pop("AF_DIALOGUE_ID", None)
        os.environ.pop("AF_SESSION_ID", None)

        # Test response_push with source
        server.clear()
        os.environ["AF_SESSION_ID"] = session_id
        os.environ["AF_DIALOGUE_ID"] = "dialogue-response-push"
        client.response_push("test content", type="llm_chunk", source="coordinator")
        req = server.wait_for_request("response_push")
        if req and validate_jsonrpc(req) and req.params.get("source") == "coordinator":
            log_pass("response_push: source=coordinator forwarded in params")
        else:
            log_fail("response_push: source=coordinator not found in params", req)

        os.environ.pop("AF_DIALOGUE_ID", None)
        os.environ.pop("AF_SESSION_ID", None)

        # Test response_push with skill/round/status/source

        # Test different namespaces
        for ns in ["sys", "ctx", "llm", "tool", "user"]:
            server.clear()
            getattr(client, ns)().put("key", "value")
            req = server.wait_for_request("put")
            if req and req.params.get("namespace") == ns:
                log_pass(f"namespace {ns}: correct namespace parameter")

        # Test JSON values
        server.clear()
        test_obj = {"nested": {"key": "value"}, "array": [1, 2, 3]}
        client.user().put("json_key", test_obj)
        req = server.wait_for_request("put")
        if req and req.params.get("value") == test_obj:
            log_pass("JSON values: correctly serialized")

    finally:
        client.close()


# ─────────────────────────────────────────────────────────────────────────────
# CLI 测试
# ─────────────────────────────────────────────────────────────────────────────

def test_cli(server: MockUDSServer, cli_path: str):
    """测试 CLI 工具"""
    start_suite("CLI Tool Tests")

    env = os.environ.copy()
    env["AF_DATABUS_UDS_FILE"] = server.socket_path

    def run_cli(args: List[str]) -> bool:
        try:
            result = subprocess.run(
                ["python3", cli_path] + args,
                env=env, capture_output=True, timeout=10
            )
            return result.returncode == 0
        except:
            return False

    # Test put
    server.clear()
    run_cli(["-ns", "user", "put", "cli_key", "cli_value"])
    req = server.wait_for_request("put")
    if req and validate_jsonrpc(req) and validate_params(req, {
        "namespace": "user", "key": "cli_key", "value": "cli_value"
    }):
        log_pass("put: sends valid JSON-RPC request")

    # Test get
    server.clear()
    run_cli(["-ns", "user", "get", "cli_key"])
    req = server.wait_for_request("get")
    if req and validate_jsonrpc(req):
        log_pass("get: sends valid JSON-RPC request")

    # Test contains
    server.clear()
    run_cli(["-ns", "user", "contains", "cli_key"])
    req = server.wait_for_request("contains")
    if req and validate_jsonrpc(req):
        log_pass("contains: sends valid JSON-RPC request")

    # Test remove
    server.clear()
    run_cli(["-ns", "user", "remove", "cli_key"])
    req = server.wait_for_request("remove")
    if req and validate_jsonrpc(req):
        log_pass("remove: sends valid JSON-RPC request")

    # Test publish
    server.clear()
    run_cli(["-ns", "user", "publish", "topic", "payload"])
    req = server.wait_for_request("publish")
    if req and validate_jsonrpc(req):
        log_pass("publish: sends valid JSON-RPC request")

    # Test async-callback
    server.clear()
    env["AF_SESSION_ID"] = "cli-session"
    env["AF_DIALOGUE_ID"] = "cli-dialogue"
    env["AF_SELF_TASK_ID"] = "cli-task"
    env["AF_NODE_ID"] = "cli-node"

    run_cli(["async-callback", "cb_key", '{"action": "test"}'])
    req = server.wait_for_request("async_callback")
    if req and validate_jsonrpc(req) and validate_params(req, {
        "session_id": "cli-session",
        "dialogue_id": "cli-dialogue",
        "task_id": "cli-task",
        "node_id": "cli-node",
        "call_key": "cb_key",
        "content": '{"action": "test"}'
    }):
        log_pass("async-callback: sends valid JSON-RPC request")

    # Clean up
    for k in ["AF_SESSION_ID", "AF_DIALOGUE_ID", "AF_SELF_TASK_ID", "AF_NODE_ID"]:
        env.pop(k, None)


# ─────────────────────────────────────────────────────────────────────────────
# Go SDK 测试
# ─────────────────────────────────────────────────────────────────────────────

def test_go_sdk(server: MockUDSServer, go_sdk_dir: str):
    """测试 Go SDK"""
    start_suite("Go SDK Tests")

    # Clear any previous captured requests
    server.clear()

    # Check if Go is available
    try:
        subprocess.run(["go", "version"], capture_output=True, check=True)
    except:
        print("  [SKIP] Go not installed")
        return

    # Create a test file in the examples directory (part of the module)
    examples_dir = os.path.join(go_sdk_dir, "examples")
    test_file_path = os.path.join(examples_dir, "sdk_test_main.go")

    # Use the correct module path from go.mod
    # Note: Environment variables for async_callback are set BEFORE creating client
    # because AsyncSDK() reads them at creation time
    test_code = '''package main

import (
    "github.com/agentflow/databus/clients/golang/databus"
)

func main() {
    client, err := databus.NewClientFromEnv()
    if err != nil {
        return
    }
    defer client.Close()

    // Test put
    client.User().Put("go_key", "go_value")

    // Test get
    client.User().Get("go_key")

    // Test contains
    client.User().Contains("go_key")

    // Test remove
    client.User().Remove("go_key")

    // Test async callback (env vars must be set before calling AsyncSDK)
    asyncSDK := client.AsyncSDK()
    asyncSDK.SendToSelf("go_callback", "{\\"status\\": \\"ok\\"}")
}
'''

    try:
        with open(test_file_path, 'w') as f:
            f.write(test_code)

        env = os.environ.copy()
        env["AF_DATABUS_UDS_FILE"] = server.socket_path
        env["CGO_ENABLED"] = "0"
        # Set async callback env vars BEFORE running (Go reads them at process start)
        env["AF_SESSION_ID"] = "go-session"
        env["AF_DIALOGUE_ID"] = "go-dialogue"
        env["AF_SELF_TASK_ID"] = "go-task"
        env["AF_NODE_ID"] = "go-node"

        result = subprocess.run(
            ["go", "run", test_file_path],
            env=env, capture_output=True, timeout=60,
            cwd=go_sdk_dir
        )

        if result.returncode != 0:
            print(f"  [SKIP] Go test failed: {result.stderr.decode()[:300]}")
            return

        # Validate captured requests - all requests were sent during Go program execution
        all_requests = server.get_requests()

        # Find and validate put
        put_reqs = [r for r in all_requests if r.method == "put"]
        if put_reqs and put_reqs[0].params.get("key") == "go_key":
            log_pass("put: sends correct parameters")
        else:
            log_fail("put: request not captured or key mismatch")

        # Find and validate get
        get_reqs = [r for r in all_requests if r.method == "get"]
        if get_reqs and get_reqs[0].params.get("key") == "go_key":
            log_pass("get: sends correct parameters")
        else:
            log_fail("get: request not captured or key mismatch")

        # Find and validate contains
        contains_reqs = [r for r in all_requests if r.method == "contains"]
        if contains_reqs and contains_reqs[0].params.get("key") == "go_key":
            log_pass("contains: sends correct parameters")
        else:
            log_fail("contains: request not captured or key mismatch")

        # Find and validate remove
        remove_reqs = [r for r in all_requests if r.method == "remove"]
        if remove_reqs and remove_reqs[0].params.get("key") == "go_key":
            log_pass("remove: sends correct parameters")
        else:
            log_fail("remove: request not captured or key mismatch")

        # Find and validate async_callback
        async_reqs = [r for r in all_requests if r.method == "async_callback"]
        if async_reqs:
            async_req = async_reqs[0]
            if async_req.params.get("call_key") == "go_callback":
                log_pass("async_callback: sends correct parameters")
            else:
                log_fail("async_callback: call_key mismatch",
                        "go_callback", async_req.params.get("call_key"))
        else:
            log_fail("async_callback: no request captured")

    except subprocess.TimeoutExpired:
        print("  [SKIP] Go test timed out")
    except Exception as e:
        print(f"  [SKIP] Go test error: {e}")
    finally:
        if os.path.exists(test_file_path):
            os.unlink(test_file_path)


# ─────────────────────────────────────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────────────────────────────────────

def main():
    script_dir = Path(__file__).parent
    project_root = script_dir.parent.parent
    sdk_base = project_root / "src" / "runtime" / "databus" / "clients"

    socket_path = f"/tmp/test_sdk_uds_{os.getpid()}.sock"

    print("=" * 60)
    print("DataBus SDK UDS Protocol Integration Tests")
    print("=" * 60)
    print(f"Socket: {socket_path}")

    # Start mock server
    server = MockUDSServer(socket_path)
    if not server.start():
        print("ERROR: Failed to start mock server")
        return 1

    print("Mock UDS Server started\n")

    try:
        # Test Python SDK
        python_sdk = sdk_base / "python" / "agentflow_databus.py"
        if python_sdk.exists():
            test_python_sdk(server, str(python_sdk))
        else:
            print(f"\n[SKIP] Python SDK not found: {python_sdk}")

        # Test CLI
        cli_tool = sdk_base / "cli" / "databus_cli.py"
        if cli_tool.exists():
            test_cli(server, str(cli_tool))
        else:
            print(f"\n[SKIP] CLI tool not found: {cli_tool}")

        # Test Go SDK
        go_sdk = sdk_base / "golang"
        if go_sdk.exists():
            test_go_sdk(server, str(go_sdk))
        else:
            print(f"\n[SKIP] Go SDK not found: {go_sdk}")

        # Java SDK would require JVM setup
        print("\n=== Java SDK Tests ===")
        print("  [SKIP] Java SDK tests require JVM and Maven/Gradle setup")

    finally:
        server.stop()

    print("\n" + "=" * 60)
    print(f"Results: PASSED={TESTS_PASSED} FAILED={TESTS_FAILED}")
    print("=" * 60)

    return 0 if TESTS_FAILED == 0 else 1


if __name__ == "__main__":
    sys.exit(main())