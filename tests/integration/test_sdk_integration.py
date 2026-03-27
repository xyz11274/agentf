#!/usr/bin/env python3
"""
SDK Integration Test Framework for DataBus UDS Protocol

This framework provides:
1. MockUDSServer - A mock server that captures all incoming JSON-RPC requests
2. RequestValidator - Validates captured requests against UDS interface design
3. TestRunner - Orchestrates tests across Python/Go/Java/CLI SDKs

Usage:
    python3 test_sdk_integration.py --sdk python
    python3 test_sdk_integration.py --sdk go
    python3 test_sdk_integration.py --sdk java
    python3 test_sdk_integration.py --sdk cli
    python3 test_sdk_integration.py --sdk all
"""

import argparse
import json
import os
import socket
import sys
import threading
import time
import subprocess
import tempfile
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Callable
from pathlib import Path


# ─────────────────────────────────────────────────────────────────────────────
# Data Models
# ─────────────────────────────────────────────────────────────────────────────

@dataclass
class CapturedRequest:
    """Represents a captured JSON-RPC request."""
    method: str
    params: Dict[str, Any]
    request_id: int
    raw_json: Dict[str, Any]
    timestamp: float = field(default_factory=time.time)

    def __repr__(self):
        return f"CapturedRequest(method={self.method}, id={self.request_id}, params={self.params})"


@dataclass
class ValidationResult:
    """Result of validating a request."""
    passed: bool
    message: str
    request: Optional[CapturedRequest] = None
    expected: Optional[Dict] = None
    actual: Optional[Dict] = None


# ─────────────────────────────────────────────────────────────────────────────
# Mock UDS Server
# ─────────────────────────────────────────────────────────────────────────────

class MockUDSServer:
    """
    A mock UDS server that captures all incoming JSON-RPC requests.

    Unlike a functional mock server, this one focuses on capturing
    and validating the request format, not simulating behavior.
    """

    def __init__(self, socket_path: str):
        self.socket_path = socket_path
        self.server_socket: Optional[socket.socket] = None
        self.running = False
        self.captured_requests: List[CapturedRequest] = []
        self.lock = threading.Lock()
        self.server_thread: Optional[threading.Thread] = None
        self.response_handlers: Dict[str, Callable] = {}
        self.default_response = {"success": True}

    def set_response(self, method: str, response: Dict):
        """Set a custom response for a specific method."""
        self.response_handlers[method] = response

    def start(self):
        """Start the mock server in a background thread."""
        if os.path.exists(self.socket_path):
            os.unlink(self.socket_path)

        self.server_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server_socket.bind(self.socket_path)
        self.server_socket.listen(10)
        self.server_socket.settimeout(1.0)
        self.running = True

        self.server_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self.server_thread.start()

        # Wait for socket to be ready
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
        """Accept connections and handle them in separate threads."""
        while self.running:
            try:
                conn, _ = self.server_socket.accept()
                threading.Thread(target=self._handle_client, args=(conn,), daemon=True).start()
            except socket.timeout:
                continue
            except Exception:
                break

    def _handle_client(self, conn: socket.socket):
        """Handle a client connection."""
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
                        self._process_request(conn, line.decode("utf-8"))
        except Exception:
            pass
        finally:
            conn.close()

    def _process_request(self, conn: socket.socket, request_str: str):
        """Process a JSON-RPC request and send response."""
        try:
            request = json.loads(request_str)
        except json.JSONDecodeError:
            response = {"jsonrpc": "2.0", "error": {"code": -32700, "message": "Parse error"}, "id": None}
            conn.sendall((json.dumps(response) + "\n").encode())
            return

        # Capture the request
        captured = CapturedRequest(
            method=request.get("method", ""),
            params=request.get("params", {}),
            request_id=request.get("id"),
            raw_json=request
        )
        with self.lock:
            self.captured_requests.append(captured)

        # Generate response
        method = request.get("method", "")
        if method in self.response_handlers:
            result = self.response_handlers[method]
        else:
            result = self._default_response_for_method(method)

        response = {"jsonrpc": "2.0", "result": result, "id": request.get("id")}
        conn.sendall((json.dumps(response) + "\n").encode())

    def _default_response_for_method(self, method: str) -> Dict:
        """Return default responses for known methods."""
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

    def get_captured_requests(self, method: Optional[str] = None) -> List[CapturedRequest]:
        """Get all captured requests, optionally filtered by method."""
        with self.lock:
            if method:
                return [r for r in self.captured_requests if r.method == method]
            return list(self.captured_requests)

    def clear_captured(self):
        """Clear all captured requests."""
        with self.lock:
            self.captured_requests.clear()

    def wait_for_request(self, method: str, timeout: float = 5.0) -> Optional[CapturedRequest]:
        """Wait for a specific method request to be captured."""
        start = time.time()
        while time.time() - start < timeout:
            with self.lock:
                for req in self.captured_requests:
                    if req.method == method:
                        return req
            time.sleep(0.1)
        return None


# ─────────────────────────────────────────────────────────────────────────────
# Request Validator
# ─────────────────────────────────────────────────────────────────────────────

class RequestValidator:
    """Validates captured requests against UDS interface design."""

    # Expected parameter schemas for each method
    METHOD_SCHEMAS = {
        "put": {
            "required": ["session_id", "namespace", "key", "value"],
            "optional": []
        },
        "get": {
            "required": ["session_id", "namespace", "key"],
            "optional": []
        },
        "remove": {
            "required": ["session_id", "namespace", "key"],
            "optional": []
        },
        "contains": {
            "required": ["session_id", "namespace", "key"],
            "optional": []
        },
        "subscribe": {
            "required": ["session_id", "namespace", "key"],
            "optional": []
        },
        "unsubscribe": {
            "required": ["subscription_id"],
            "optional": []
        },
        "publish": {
            "required": ["session_id", "namespace", "topic", "value"],
            "optional": []
        },
        "list_keys": {
            "required": ["session_id", "namespace"],
            "optional": ["prefix"]
        },
        "async_callback": {
            "required": ["session_id", "dialogue_id", "task_id", "node_id", "call_key", "content"],
            "optional": []
        }
    }

    VALID_NAMESPACES = ["sys", "ctx", "llm", "tool", "user"]

    @classmethod
    def validate_jsonrpc_format(cls, request: CapturedRequest) -> ValidationResult:
        """Validate JSON-RPC 2.0 format."""
        raw = request.raw_json

        # Check jsonrpc version
        if raw.get("jsonrpc") != "2.0":
            return ValidationResult(
                passed=False,
                message="jsonrpc field must be '2.0'",
                request=request,
                expected={"jsonrpc": "2.0"},
                actual={"jsonrpc": raw.get("jsonrpc")}
            )

        # Check method
        if not raw.get("method"):
            return ValidationResult(
                passed=False,
                message="method field is required",
                request=request
            )

        # Check params
        if "params" not in raw:
            return ValidationResult(
                passed=False,
                message="params field is required",
                request=request
            )

        # Check id (should be present for requests, not notifications)
        if raw.get("id") is None:
            return ValidationResult(
                passed=False,
                message="id field is required for requests",
                request=request
            )

        return ValidationResult(passed=True, message="Valid JSON-RPC 2.0 format", request=request)

    @classmethod
    def validate_method_params(cls, request: CapturedRequest) -> ValidationResult:
        """Validate method-specific parameters."""
        method = request.method
        params = request.params

        if method not in cls.METHOD_SCHEMAS:
            return ValidationResult(
                passed=False,
                message=f"Unknown method: {method}",
                request=request
            )

        schema = cls.METHOD_SCHEMAS[method]
        required = schema["required"]

        # Check required parameters
        missing = []
        for param in required:
            if param not in params:
                missing.append(param)

        if missing:
            return ValidationResult(
                passed=False,
                message=f"Missing required parameters: {missing}",
                request=request,
                expected={"required": required},
                actual=list(params.keys())
            )

        return ValidationResult(
            passed=True,
            message=f"Valid parameters for {method}",
            request=request
        )

    @classmethod
    def validate_namespace(cls, request: CapturedRequest, expected_namespace: str) -> ValidationResult:
        """Validate namespace parameter."""
        params = request.params
        namespace = params.get("namespace")

        if namespace != expected_namespace:
            return ValidationResult(
                passed=False,
                message=f"Namespace mismatch",
                request=request,
                expected={"namespace": expected_namespace},
                actual={"namespace": namespace}
            )

        if namespace not in cls.VALID_NAMESPACES:
            return ValidationResult(
                passed=False,
                message=f"Invalid namespace: {namespace}",
                request=request,
                expected={"valid_namespaces": cls.VALID_NAMESPACES},
                actual={"namespace": namespace}
            )

        return ValidationResult(passed=True, message=f"Valid namespace: {namespace}", request=request)

    @classmethod
    def validate_session_id(cls, request: CapturedRequest, expected_session: str) -> ValidationResult:
        """Validate session_id parameter."""
        session_id = request.params.get("session_id")

        if session_id != expected_session:
            return ValidationResult(
                passed=False,
                message="session_id mismatch",
                request=request,
                expected={"session_id": expected_session},
                actual={"session_id": session_id}
            )

        return ValidationResult(passed=True, message=f"Valid session_id: {session_id}", request=request)

    @classmethod
    def validate_key(cls, request: CapturedRequest, expected_key: str) -> ValidationResult:
        """Validate key parameter."""
        key = request.params.get("key")

        if key != expected_key:
            return ValidationResult(
                passed=False,
                message="key mismatch",
                request=request,
                expected={"key": expected_key},
                actual={"key": key}
            )

        return ValidationResult(passed=True, message=f"Valid key: {key}", request=request)

    @classmethod
    def validate_value(cls, request: CapturedRequest, expected_value: Any) -> ValidationResult:
        """Validate value parameter."""
        value = request.params.get("value")

        if value != expected_value:
            return ValidationResult(
                passed=False,
                message="value mismatch",
                request=request,
                expected={"value": expected_value},
                actual={"value": value}
            )

        return ValidationResult(passed=True, message=f"Valid value: {value}", request=request)

    @classmethod
    def validate_async_callback_params(cls, request: CapturedRequest,
                                        session_id: str, dialogue_id: str,
                                        task_id: str, node_id: str,
                                        call_key: str, content: str) -> ValidationResult:
        """Validate async_callback specific parameters."""
        params = request.params

        checks = [
            ("session_id", session_id),
            ("dialogue_id", dialogue_id),
            ("task_id", task_id),
            ("node_id", node_id),
            ("call_key", call_key),
            ("content", content)
        ]

        for param_name, expected in checks:
            actual = params.get(param_name)
            if actual != expected:
                return ValidationResult(
                    passed=False,
                    message=f"{param_name} mismatch",
                    request=request,
                    expected={param_name: expected},
                    actual={param_name: actual}
                )

        return ValidationResult(passed=True, message="Valid async_callback parameters", request=request)


# ─────────────────────────────────────────────────────────────────────────────
# Test Framework Base
# ─────────────────────────────────────────────────────────────────────────────

class SDKTestRunner:
    """Base class for SDK test runners."""

    def __init__(self, mock_server: MockUDSServer, session_id: str = "test-session-123"):
        self.server = mock_server
        self.session_id = session_id
        self.test_results: List[Dict] = []

    def record_result(self, test_name: str, passed: bool, message: str, details: Optional[Dict] = None):
        """Record a test result."""
        result = {
            "test": test_name,
            "passed": passed,
            "message": message,
            "details": details or {}
        }
        self.test_results.append(result)
        status = "PASS" if passed else "FAIL"
        print(f"  [{status}] {test_name}: {message}")
        if not passed and details:
            print(f"        Expected: {details.get('expected')}")
            print(f"        Actual: {details.get('actual')}")

    def setup(self):
        """Override to set up SDK-specific environment."""
        pass

    def teardown(self):
        """Override to clean up SDK-specific environment."""
        pass

    def run_tests(self) -> bool:
        """Run all tests. Override in subclasses."""
        raise NotImplementedError


# ─────────────────────────────────────────────────────────────────────────────
# Python SDK Test Runner
# ─────────────────────────────────────────────────────────────────────────────

class PythonSDKTestRunner(SDKTestRunner):
    """Test runner for Python SDK."""

    def __init__(self, mock_server: MockUDSServer, sdk_path: str, session_id: str = "test-session-123"):
        super().__init__(mock_server, session_id)
        self.sdk_path = sdk_path
        self.client = None

    def setup(self):
        """Import the SDK."""
        sys.path.insert(0, os.path.dirname(self.sdk_path))
        from agentflow_databus import DataBusClient
        self.DataBusClient = DataBusClient

    def run_tests(self) -> bool:
        """Run Python SDK tests."""
        print("\n=== Python SDK Tests ===")

        # Create client
        self.client = self.DataBusClient(self.server.socket_path, self.session_id)

        try:
            self._test_put()
            self._test_get()
            self._test_remove()
            self._test_contains()
            self._test_subscribe()
            self._test_unsubscribe()
            self._test_publish()
            self._test_async_callback()
            self._test_namespaces()
            self._test_json_values()
        finally:
            self.client.close()

        return all(r["passed"] for r in self.test_results)

    def _test_put(self):
        """Test put method."""
        self.server.clear_captured()
        self.client.user().put("test_key", "test_value")

        requests = self.server.get_captured_requests("put")
        if not requests:
            self.record_result("put_method", False, "No request captured")
            return

        req = requests[0]

        # Validate JSON-RPC format
        result = RequestValidator.validate_jsonrpc_format(req)
        if not result.passed:
            self.record_result("put_jsonrpc", False, result.message, {"expected": result.expected, "actual": result.actual})
            return

        # Validate method params
        result = RequestValidator.validate_method_params(req)
        if not result.passed:
            self.record_result("put_params", False, result.message, {"expected": result.expected, "actual": result.actual})
            return

        # Validate specific values
        result = RequestValidator.validate_namespace(req, "user")
        if not result.passed:
            self.record_result("put_namespace", False, result.message, {"expected": result.expected, "actual": result.actual})
            return

        result = RequestValidator.validate_key(req, "test_key")
        if not result.passed:
            self.record_result("put_key", False, result.message, {"expected": result.expected, "actual": result.actual})
            return

        result = RequestValidator.validate_value(req, "test_value")
        if not result.passed:
            self.record_result("put_value", False, result.message, {"expected": result.expected, "actual": result.actual})
            return

        self.record_result("put_method", True, "put method sends valid JSON-RPC request")

    def _test_get(self):
        """Test get method."""
        self.server.clear_captured()
        self.client.user().get("test_key")

        requests = self.server.get_captured_requests("get")
        if not requests:
            self.record_result("get_method", False, "No request captured")
            return

        req = requests[0]
        result = RequestValidator.validate_jsonrpc_format(req)
        if not result.passed:
            self.record_result("get_jsonrpc", False, result.message, {"expected": result.expected, "actual": result.actual})
            return

        result = RequestValidator.validate_method_params(req)
        if not result.passed:
            self.record_result("get_params", False, result.message, {"expected": result.expected, "actual": result.actual})
            return

        # get should NOT have value parameter
        if "value" in req.params:
            self.record_result("get_no_value", False, "get should not include value parameter")
            return

        self.record_result("get_method", True, "get method sends valid JSON-RPC request")

    def _test_remove(self):
        """Test remove method."""
        self.server.clear_captured()
        self.client.user().remove("test_key")

        requests = self.server.get_captured_requests("remove")
        if not requests:
            self.record_result("remove_method", False, "No request captured")
            return

        req = requests[0]
        result = RequestValidator.validate_method_params(req)
        if not result.passed:
            self.record_result("remove_params", False, result.message)
            return

        self.record_result("remove_method", True, "remove method sends valid JSON-RPC request")

    def _test_contains(self):
        """Test contains method."""
        self.server.clear_captured()
        self.client.user().contains("test_key")

        requests = self.server.get_captured_requests("contains")
        if not requests:
            self.record_result("contains_method", False, "No request captured")
            return

        req = requests[0]
        result = RequestValidator.validate_method_params(req)
        if not result.passed:
            self.record_result("contains_params", False, result.message)
            return

        self.record_result("contains_method", True, "contains method sends valid JSON-RPC request")

    def _test_subscribe(self):
        """Test subscribe method."""
        self.server.clear_captured()
        self.client.user().subscribe("test_key", lambda k, v: None)

        requests = self.server.get_captured_requests("subscribe")
        if not requests:
            self.record_result("subscribe_method", False, "No request captured")
            return

        req = requests[0]
        result = RequestValidator.validate_method_params(req)
        if not result.passed:
            self.record_result("subscribe_params", False, result.message)
            return

        self.record_result("subscribe_method", True, "subscribe method sends valid JSON-RPC request")

    def _test_unsubscribe(self):
        """Test unsubscribe method."""
        self.server.clear_captured()
        self.client.user().unsubscribe(1)

        requests = self.server.get_captured_requests("unsubscribe")
        if not requests:
            self.record_result("unsubscribe_method", False, "No request captured")
            return

        req = requests[0]
        if req.params.get("subscription_id") != 1:
            self.record_result("unsubscribe_id", False, "subscription_id mismatch")
            return

        self.record_result("unsubscribe_method", True, "unsubscribe method sends valid JSON-RPC request")

    def _test_publish(self):
        """Test publish method."""
        self.server.clear_captured()
        self.client.user().publish("test_topic", "test_payload")

        requests = self.server.get_captured_requests("publish")
        if not requests:
            self.record_result("publish_method", False, "No request captured")
            return

        req = requests[0]
        result = RequestValidator.validate_method_params(req)
        if not result.passed:
            self.record_result("publish_params", False, result.message)
            return

        if req.params.get("topic") != "test_topic":
            self.record_result("publish_topic", False, "topic mismatch")
            return

        self.record_result("publish_method", True, "publish method sends valid JSON-RPC request")

    def _test_async_callback(self):
        """Test async_callback method."""
        self.server.clear_captured()

        # Set environment variables
        os.environ["AF_SESSION_ID"] = self.session_id
        os.environ["AF_DIALOGUE_ID"] = "dialogue-456"
        os.environ["AF_SELF_TASK_ID"] = "task-789"
        os.environ["AF_NODE_ID"] = "node-101"

        try:
            async_sdk = self.client.async_sdk()
            async_sdk.send_to_self("callback_key", '{"status": "ok"}')

            requests = self.server.get_captured_requests("async_callback")
            if not requests:
                self.record_result("async_callback_method", False, "No request captured")
                return

            req = requests[0]

            # Validate all async_callback parameters
            result = RequestValidator.validate_async_callback_params(
                req,
                session_id=self.session_id,
                dialogue_id="dialogue-456",
                task_id="task-789",
                node_id="node-101",
                call_key="callback_key",
                content='{"status": "ok"}'
            )
            if not result.passed:
                self.record_result("async_callback_params", False, result.message, {"expected": result.expected, "actual": result.actual})
                return

            self.record_result("async_callback_method", True, "async_callback sends valid JSON-RPC request")
        finally:
            # Clean up env vars
            for key in ["AF_SESSION_ID", "AF_DIALOGUE_ID", "AF_SELF_TASK_ID", "AF_NODE_ID"]:
                os.environ.pop(key, None)

    def _test_namespaces(self):
        """Test different namespaces."""
        self.server.clear_captured()

        namespaces = ["sys", "ctx", "llm", "tool", "user"]
        for ns in namespaces:
            self.server.clear_captured()
            ns_client = getattr(self.client, ns)()
            ns_client.put("key", "value")

            requests = self.server.get_captured_requests("put")
            if not requests:
                self.record_result(f"namespace_{ns}", False, f"No request for {ns}")
                return

            req = requests[0]
            if req.params.get("namespace") != ns:
                self.record_result(f"namespace_{ns}", False, f"Namespace mismatch: expected {ns}, got {req.params.get('namespace')}")
                return

        self.record_result("namespaces", True, "All namespaces send correct namespace parameter")

    def _test_json_values(self):
        """Test JSON values are correctly serialized."""
        self.server.clear_captured()

        test_value = {"nested": {"key": "value"}, "array": [1, 2, 3], "number": 42}
        self.client.user().put("json_key", test_value)

        requests = self.server.get_captured_requests("put")
        if not requests:
            self.record_result("json_values", False, "No request captured")
            return

        req = requests[0]
        if req.params.get("value") != test_value:
            self.record_result("json_values", False, "JSON value not correctly serialized",
                             {"expected": test_value, "actual": req.params.get("value")})
            return

        self.record_result("json_values", True, "JSON values are correctly serialized")


# ─────────────────────────────────────────────────────────────────────────────
# CLI Test Runner
# ─────────────────────────────────────────────────────────────────────────────

class CLITestRunner(SDKTestRunner):
    """Test runner for CLI tool."""

    def __init__(self, mock_server: MockUDSServer, cli_path: str, session_id: str = "test-session-123"):
        super().__init__(mock_server, session_id)
        self.cli_path = cli_path

    def run_tests(self) -> bool:
        """Run CLI tests."""
        print("\n=== CLI Tool Tests ===")

        env = os.environ.copy()
        env["AF_DATABUS_UDS_FILE"] = self.server.socket_path

        self._test_cli_put(env)
        self._test_cli_get(env)
        self._test_cli_remove(env)
        self._test_cli_contains(env)
        self._test_cli_publish(env)
        self._test_cli_async_callback(env)

        return all(r["passed"] for r in self.test_results)

    def _run_cli(self, args: List[str], env: Dict) -> bool:
        """Run CLI command and return success."""
        try:
            result = subprocess.run(
                ["python3", self.cli_path] + args,
                env=env,
                capture_output=True,
                timeout=10
            )
            return result.returncode == 0
        except Exception:
            return False

    def _test_cli_put(self, env: Dict):
        """Test CLI put command."""
        self.server.clear_captured()
        self._run_cli(["-ns", "user", "put", "cli_key", "cli_value"], env)

        requests = self.server.get_captured_requests("put")
        if not requests:
            self.record_result("cli_put", False, "No request captured")
            return

        req = requests[0]
        if req.params.get("key") != "cli_key" or req.params.get("value") != "cli_value":
            self.record_result("cli_put", False, "Parameters mismatch",
                             {"expected": {"key": "cli_key", "value": "cli_value"},
                              "actual": {"key": req.params.get("key"), "value": req.params.get("value")}})
            return

        self.record_result("cli_put", True, "CLI put sends valid request")

    def _test_cli_get(self, env: Dict):
        """Test CLI get command."""
        self.server.clear_captured()
        self._run_cli(["-ns", "user", "get", "cli_key"], env)

        requests = self.server.get_captured_requests("get")
        if not requests:
            self.record_result("cli_get", False, "No request captured")
            return

        self.record_result("cli_get", True, "CLI get sends valid request")

    def _test_cli_remove(self, env: Dict):
        """Test CLI remove command."""
        self.server.clear_captured()
        self._run_cli(["-ns", "user", "remove", "cli_key"], env)

        requests = self.server.get_captured_requests("remove")
        if not requests:
            self.record_result("cli_remove", False, "No request captured")
            return

        self.record_result("cli_remove", True, "CLI remove sends valid request")

    def _test_cli_contains(self, env: Dict):
        """Test CLI contains command."""
        self.server.clear_captured()
        self._run_cli(["-ns", "user", "contains", "cli_key"], env)

        requests = self.server.get_captured_requests("contains")
        if not requests:
            self.record_result("cli_contains", False, "No request captured")
            return

        self.record_result("cli_contains", True, "CLI contains sends valid request")

    def _test_cli_publish(self, env: Dict):
        """Test CLI publish command."""
        self.server.clear_captured()
        self._run_cli(["-ns", "user", "publish", "test_topic", "payload"], env)

        requests = self.server.get_captured_requests("publish")
        if not requests:
            self.record_result("cli_publish", False, "No request captured")
            return

        self.record_result("cli_publish", True, "CLI publish sends valid request")

    def _test_cli_async_callback(self, env: Dict):
        """Test CLI async-callback command."""
        self.server.clear_captured()

        # Set required environment variables
        env["AF_SESSION_ID"] = self.session_id
        env["AF_DIALOGUE_ID"] = "dialogue-cli"
        env["AF_SELF_TASK_ID"] = "task-cli"
        env["AF_NODE_ID"] = "node-cli"

        self._run_cli(["async-callback", "callback_key", '{"action": "test"}'], env)

        requests = self.server.get_captured_requests("async_callback")
        if not requests:
            self.record_result("cli_async_callback", False, "No request captured")
            return

        req = requests[0]
        result = RequestValidator.validate_async_callback_params(
            req,
            session_id=self.session_id,
            dialogue_id="dialogue-cli",
            task_id="task-cli",
            node_id="node-cli",
            call_key="callback_key",
            content='{"action": "test"}'
        )

        if not result.passed:
            self.record_result("cli_async_callback", False, result.message,
                             {"expected": result.expected, "actual": result.actual})
            return

        self.record_result("cli_async_callback", True, "CLI async-callback sends valid request")


# ─────────────────────────────────────────────────────────────────────────────
# Go SDK Test Runner
# ─────────────────────────────────────────────────────────────────────────────

class GoSDKTestRunner(SDKTestRunner):
    """Test runner for Go SDK."""

    def __init__(self, mock_server: MockUDSServer, go_sdk_dir: str, session_id: str = "test-session-123"):
        super().__init__(mock_server, session_id)
        self.go_sdk_dir = go_sdk_dir

    def run_tests(self) -> bool:
        """Run Go SDK tests."""
        print("\n=== Go SDK Tests ===")

        # Create a test program
        test_program = self._create_test_program()
        test_file = tempfile.NamedTemporaryFile(mode='w', suffix='.go', delete=False)
        test_file.write(test_program)
        test_file.close()

        try:
            # Run the Go test
            env = os.environ.copy()
            env["AF_DATABUS_UDS_FILE"] = self.server.socket_path
            env["CGO_ENABLED"] = "0"

            # Change to go mod directory or create temp module
            result = subprocess.run(
                ["go", "run", test_file.name],
                env=env,
                capture_output=True,
                timeout=30,
                cwd=self.go_sdk_dir
            )

            if result.returncode != 0:
                self.record_result("go_sdk", False, f"Go test failed: {result.stderr.decode()}")
                return False

            # Validate captured requests
            self._validate_go_requests()

        except FileNotFoundError:
            self.record_result("go_sdk", False, "Go not installed or test file not found")
            return False
        except Exception as e:
            self.record_result("go_sdk", False, f"Error running Go tests: {e}")
            return False
        finally:
            os.unlink(test_file.name)

        return all(r["passed"] for r in self.test_results)

    def _create_test_program(self) -> str:
        """Create a Go test program that exercises the SDK."""
        return f'''package main

import (
    "fmt"
    "os"
    "databus"
)

func main() {{
    client, err := databus.NewClientFromEnv()
    if err != nil {{
        fmt.Fprintf(os.Stderr, "Failed to connect: %v\\n", err)
        os.Exit(1)
    }}
    defer client.Close()

    // Test put
    client.User().Put("go_key", "go_value")

    // Test get
    client.User().Get("go_key")

    // Test contains
    client.User().Contains("go_key")

    // Test remove
    client.User().Remove("go_key")

    // Test publish
    client.User().Publish("go_topic", "go_payload")

    // Test async callback
    os.Setenv("AF_SESSION_ID", "{self.session_id}")
    os.Setenv("AF_DIALOGUE_ID", "go-dialogue")
    os.Setenv("AF_SELF_TASK_ID", "go-task")
    os.Setenv("AF_NODE_ID", "go-node")

    asyncSDK := client.AsyncSDK()
    asyncSDK.SendToSelf("go_callback", "{{\\"status\\": \\"ok\\"}}")

    fmt.Println("Go SDK test completed")
}}
'''

    def _validate_go_requests(self):
        """Validate captured Go SDK requests."""
        # Validate put
        put_reqs = self.server.get_captured_requests("put")
        if put_reqs:
            req = put_reqs[0]
            if req.params.get("key") == "go_key" and req.params.get("value") == "go_value":
                self.record_result("go_put", True, "Go put sends correct parameters")
            else:
                self.record_result("go_put", False, "Go put parameter mismatch")
        else:
            self.record_result("go_put", False, "No put request captured")

        # Validate async_callback
        async_reqs = self.server.get_captured_requests("async_callback")
        if async_reqs:
            req = async_reqs[0]
            result = RequestValidator.validate_async_callback_params(
                req,
                session_id=self.session_id,
                dialogue_id="go-dialogue",
                task_id="go-task",
                node_id="go-node",
                call_key="go_callback",
                content='{"status": "ok"}'
            )
            if result.passed:
                self.record_result("go_async_callback", True, "Go async_callback sends correct parameters")
            else:
                self.record_result("go_async_callback", False, result.message)
        else:
            self.record_result("go_async_callback", False, "No async_callback request captured")


# ─────────────────────────────────────────────────────────────────────────────
# Main Test Runner
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="SDK Integration Tests for DataBus UDS Protocol")
    parser.add_argument("--sdk", choices=["python", "go", "java", "cli", "all"], default="all",
                        help="SDK to test (default: all)")
    parser.add_argument("--socket", default=None, help="Custom socket path")
    args = parser.parse_args()

    # Set up paths
    script_dir = Path(__file__).parent
    project_root = script_dir.parent.parent
    sdk_base = project_root / "src" / "runtime" / "databus" / "clients"

    socket_path = args.socket or f"/tmp/test_sdk_integration_{os.getpid()}.sock"

    print("=" * 60)
    print("DataBus SDK Integration Tests")
    print("Testing UDS JSON-RPC 2.0 Protocol Compliance")
    print("=" * 60)
    print(f"Socket: {socket_path}")
    print(f"Session ID: test-session-123")

    # Start mock server
    server = MockUDSServer(socket_path)
    if not server.start():
        print("ERROR: Failed to start mock server")
        return 1

    print("Mock UDS Server started")

    try:
        all_passed = True

        if args.sdk in ["python", "all"]:
            python_sdk = sdk_base / "python" / "agentflow_databus.py"
            if python_sdk.exists():
                runner = PythonSDKTestRunner(server, str(python_sdk))
                runner.setup()
                if not runner.run_tests():
                    all_passed = False
            else:
                print(f"\nWARNING: Python SDK not found at {python_sdk}")

        if args.sdk in ["cli", "all"]:
            cli_tool = sdk_base / "cli" / "databus_cli.py"
            if cli_tool.exists():
                runner = CLITestRunner(server, str(cli_tool))
                if not runner.run_tests():
                    all_passed = False
            else:
                print(f"\nWARNING: CLI tool not found at {cli_tool}")

        if args.sdk in ["go", "all"]:
            go_sdk = sdk_base / "golang" / "databus"
            if go_sdk.exists():
                runner = GoSDKTestRunner(server, str(go_sdk))
                if not runner.run_tests():
                    all_passed = False
            else:
                print(f"\nWARNING: Go SDK not found at {go_sdk}")

        if args.sdk in ["java", "all"]:
            print("\n=== Java SDK Tests ===")
            print("WARNING: Java SDK tests require Maven/Gradle setup - skipping")
            # Java tests would require compiling and running a test class
            # which is more complex to set up in this framework

        print("\n" + "=" * 60)
        if all_passed:
            print("All tests PASSED")
            return 0
        else:
            print("Some tests FAILED")
            return 1

    finally:
        server.stop()
        print("\nMock UDS Server stopped")


if __name__ == "__main__":
    sys.exit(main())