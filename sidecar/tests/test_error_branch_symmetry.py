#!/usr/bin/env python3
"""
sidecar 异步路径错误四分档对称性护测（dev 模式 httplib 路径）。

护测目标：锁住 dev `forward_to_bridge` 异步路径的错误四分档与生产行为对称
（对齐 sidecar_router.cpp:418-475 的错误分流）。一旦 dev/prod 任一端改了
错误处理分支而另一端没同步，本测试立即报警——防止 dev/prod 行为分叉。

四分档（dev 与 prod 必须对称）：
  ① bridge 不可达/连接拒绝 → ts=500/BridgeError，跳过 Completed，发错误 TaskStatus
  ② bridge HTTP 非 200     → ts=resp.status/BridgeError，body 提取 error detail
  ③ bridge 200 但 SSE 流内 error payload → ts=500/BridgeError，error_message 携详情
  ④ bridge 200 无错误      → ts=200/TaskCompleted，先发 Completed UserOutput 再发 TaskStatus

测试拓扑（真实集成，不 mock 关键集成点—— F13 铁律）：
  AccessCtrl POST → sidecar (:23810) → mock Bridge (:23811) SSE 流
                                    └→ POST 回调 → mock callback server (:23812)

  sidecar 走 dev 模式（COREMIND_ENABLED=OFF stub 编译的 coremind_bridge），
  "--bridge-url" 指向 mock Bridge，SysAgentEntity 指向 mock callback。

每个分档独立验证：
  - sidecar 入口应答 200 无 body（异步模式契约 B2）
  - 回调队列收到 TaskStatusReport，TaskStatusCode/TaskStatusReport/UserOutputText 符合分档预期
  - 错误分档（①②③）：不应收到 StreamStatus=Completed 的 UserOutput（避免虚假完成信号）
  - 成功分档（④）：应收到 Completed UserOutput + 200/TaskCompleted TaskStatus

输出格式（兼容 regression_test.sh 的 run_legacy_e2e_binary）：
  Tests passed: N
  Tests failed: N
  exit code: 0=全过，1=有失败

用法：
  python3 sidecar/tests/test_error_branch_symmetry.py
"""

from __future__ import annotations

import json
import signal
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from typing import Any

# ── 拓扑端口（避开 test_forward_async_e2e.py 的 23800-23802）──────────
SIDECAR_BIN = Path(__file__).resolve().parent.parent / "build-dev" / "coremind_bridge"
SIDECAR_PORT = 23810
BRIDGE_PORT = 23811
CALLBACK_PORT = 23812
SIDECAR_URL = f"http://127.0.0.1:{SIDECAR_PORT}"
BRIDGE_URL = f"http://127.0.0.1:{BRIDGE_PORT}"
CALLBACK_HOST = f"127.0.0.1:{CALLBACK_PORT}"

# 回调收集（按场景隔离，通过 ScenarioContextHolder 切换 handler 引用）
_callbacks: list[dict[str, Any]] = []
_lock = threading.Lock()


# ── mock Bridge：按场景返回不同 SSE 流 / HTTP 状态 ─────────────────
# 通过类变量 ScenarioContextHolder.current 控制本次 POST 应返回什么
class _ScenarioContext:
    """控制 mock Bridge 本次 POST 应返回什么响应。"""

    current: dict[str, Any] = {}  # {status, sse_chunks, body}

    @classmethod
    def set(cls, scenario: dict[str, Any]) -> None:
        cls.current = scenario

    @classmethod
    def take(cls) -> dict[str, Any]:
        return cls.current


class MockBridgeHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        scenario = _ScenarioContext.take()
        status = scenario.get("status", 200)
        body_bytes = scenario.get("body", b"")
        if isinstance(body_bytes, str):
            body_bytes = body_bytes.encode()

        if status == 200:
            sse_chunks = scenario.get("sse_chunks", [])
            sse = "".join(
                f"data: {json.dumps(c, ensure_ascii=False)}\n\n" for c in sse_chunks
            )
            if scenario.get("append_done", True):
                sse += "data: [DONE]\n\n"
            resp = sse.encode()
            self.send_response(200)
            self.send_header("content-type", "text/event-stream")
            self.send_header("content-length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)
        else:
            # 非 200 状态码：返回 JSON error body（模拟 bridge HTTPException）
            self.send_response(status)
            self.send_header("content-type", "application/json")
            self.send_header("content-length", str(len(body_bytes)))
            self.end_headers()
            self.wfile.write(body_bytes)

    def log_message(self, *args):  # 静默
        pass


# ── mock callback server：收集 sidecar 回调 ───────────────────────
class MockCallbackHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("content-length", 0))
        body = self.rfile.read(length)
        try:
            parsed = json.loads(body)
        except json.JSONDecodeError:
            parsed = {"_raw": body.decode(errors="replace")}
        with _lock:
            _callbacks.append(parsed)
        resp = b'{"status":"ok"}'
        self.send_response(200)
        self.send_header("content-length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

    def log_message(self, *args):  # 静默
        pass


# ── 工具函数 ───────────────────────────────────────────────────
def _start_server(handler, port) -> HTTPServer:
    server = HTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def _http_post(url: str, body: dict) -> tuple[int, str]:
    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return resp.status, resp.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def _wait_callbacks(min_count: int, timeout: float = 8.0) -> int:
    deadline = time.time() + timeout
    while time.time() < deadline:
        with _lock:
            n = len(_callbacks)
        if n >= min_count:
            return n
        time.sleep(0.05)
    with _lock:
        return len(_callbacks)


def _reset_callbacks() -> None:
    with _lock:
        _callbacks.clear()


def _accessctrl() -> dict[str, Any]:
    """构造合法的 AccessCtrl 任务下发请求体（带 SysAgentEntity 触发异步路径）。"""
    return {
        "Header": {
            "MessageType": "UserTaskManagement",
            "Sender": "UpstreamMock",
            "Receiver": "SysAgent",
            "UserName": "sym-test-user",
            "DeviceId": "sym-test-device",
            "DeviceCategory": "2",
            "UUID": "sym-test-uuid",
            "ClientSessionId": "sym-test-session",
            "TaskID": "sym-test-task",
            "DialogueId": "sym-test-dialogue",
            "AcmSessionId": "sym-test-acm",
            "SessionId": "sym-test-session-id",
            "SeqNum": "sym-test-seq",
            "SysAgentEntity": CALLBACK_HOST,
            "EventTimeStamp": "2026-08-03T00:00:00Z",
        },
        "Data": {
            "MessageType": "TaskAssignment",
            "UserInputText": "帮我推荐穿搭",
        },
    }


# ── 单分档校验：断言 TaskStatusReport 符合预期 + Completed UserOutput 是否存在 ──
def _assert_task_status(
    failures: list[str],
    scenario_name: str,
    expected_ts_code: int,
    expected_ts_status: str,
    should_have_completed: bool,
    expected_text_prefix: str = "",
) -> None:
    """校验回调队列中的 TaskStatusReport + Completed UserOutput 符合分档预期。"""
    with _lock:
        snapshot = list(_callbacks)

    task_statuses = [
        c for c in snapshot
        if c.get("Data", {}).get("MessageType") == "TaskStatusReport"
    ]
    if not task_statuses:
        failures.append(f"[{scenario_name}] 未收到 TaskStatusReport 回调")
        return

    ts = task_statuses[-1].get("Data", {})
    if ts.get("TaskStatusCode") != expected_ts_code:
        failures.append(
            f"[{scenario_name}] TaskStatusCode 应={expected_ts_code}，"
            f"实际={ts.get('TaskStatusCode')}"
        )
    if ts.get("TaskStatusReport") != expected_ts_status:
        failures.append(
            f"[{scenario_name}] TaskStatusReport 应={expected_ts_status!r}，"
            f"实际={ts.get('TaskStatusReport')!r}"
        )
    if expected_text_prefix:
        actual_text = ts.get("UserOutputText", "")
        if not actual_text.startswith(expected_text_prefix):
            failures.append(
                f"[{scenario_name}] UserOutputText 应以 {expected_text_prefix!r} 开头，"
                f"实际={actual_text[:120]!r}"
            )

    completed = [
        c for c in snapshot
        if c.get("Data", {}).get("MessageType") == "UserOutput"
        and c.get("Data", {}).get("StreamStatus") == "Completed"
    ]
    if should_have_completed and not completed:
        failures.append(
            f"[{scenario_name}] 应收到 Completed UserOutput，但未收到"
        )
    if not should_have_completed and completed:
        failures.append(
            f"[{scenario_name}] 不应收到 Completed UserOutput（错误分档跳过），"
            f"但收到了 {len(completed)} 个"
        )


# ── 各分档场景 ─────────────────────────────────────────────────
def scenario_1_bridge_unreachable() -> dict[str, Any]:
    """分档①：Bridge 不可达。mock Bridge 不启动，sidecar 连接被拒。"""
    return {
        "name": "1_bridge_unreachable",
        "bridge_start": False,
        "expected_ts_code": 500,
        "expected_ts_status": "BridgeError",
        "should_have_completed": False,
        "expected_text_prefix": "bridge_error:",
    }


def scenario_2_http_non_200() -> dict[str, Any]:
    """分档②：Bridge 返回 HTTP 404。模拟 agent 不存在。"""
    return {
        "name": "2_http_404",
        "bridge_start": True,
        "bridge_scenario": {
            "status": 404,
            "body": json.dumps({"detail": "agent not found: outfit-assistant"}),
        },
        "expected_ts_code": 404,
        "expected_ts_status": "BridgeError",
        "should_have_completed": False,
        "expected_text_prefix": "bridge_error:",
    }


def scenario_3_sse_error_payload() -> dict[str, Any]:
    """分档③：Bridge 200 + SSE 流内 error payload。模拟 daemon 通知中途失败。"""
    return {
        "name": "3_sse_error_payload",
        "bridge_start": True,
        "bridge_scenario": {
            "status": 200,
            "sse_chunks": [
                {"choices": [{"delta": {"content": "正在"}}],
                 "_source": "coordinator", "_chunk_type": "llm_chunk"},
                {"error": {"message": "agent.execute timed out after 30s",
                           "code": -32000}},
            ],
            "append_done": True,
        },
        "expected_ts_code": 500,
        "expected_ts_status": "BridgeError",
        "should_have_completed": False,
        "expected_text_prefix": "bridge_error:",
    }


def scenario_4_success() -> dict[str, Any]:
    """分档④：Bridge 200 + 正常 SSE 流。对齐 happy path。"""
    return {
        "name": "4_success",
        "bridge_start": True,
        "bridge_scenario": {
            "status": 200,
            "sse_chunks": [
                {"choices": [{"delta": {"content": "你好"}}],
                 "_source": "coordinator", "_chunk_type": "llm_chunk"},
                {"choices": [{"delta": {}}], "finish_reason": "stop",
                 "_source": "coordinator",
                 "artifacts": [{"type": "image", "url": "http://x/look.png"}]},
            ],
            "append_done": True,
        },
        "expected_ts_code": 200,
        "expected_ts_status": "TaskCompleted",
        "should_have_completed": True,
        # 成功时 UserOutputText 是 coordinator 的最终回答，不是 bridge_error: 前缀
        "expected_text_prefix": "",
    }


ALL_SCENARIOS = [
    scenario_1_bridge_unreachable(),
    scenario_2_http_non_200(),
    scenario_3_sse_error_payload(),
    scenario_4_success(),
]


# ── 主流程 ─────────────────────────────────────────────────────
def main() -> int:
    if not SIDECAR_BIN.exists():
        print(f"[FAIL] sidecar binary missing: {SIDECAR_BIN}")
        print("Tests passed: 0")
        print("Tests failed: 1")
        return 1

    bridge_srv: HTTPServer | None = None
    cb_srv = _start_server(MockCallbackHandler, CALLBACK_PORT)

    sidecar = subprocess.Popen(
        [str(SIDECAR_BIN), "--listen", f"127.0.0.1:{SIDECAR_PORT}",
         "--bridge-url", BRIDGE_URL],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )

    failures: list[str] = []
    passed_count = 0
    failed_count = 0

    try:
        # 轮询 /health 直到 sidecar 就绪
        ready = False
        for _ in range(40):
            if sidecar.poll() is not None:
                break
            try:
                with urllib.request.urlopen(
                    f"{SIDECAR_URL}/health", timeout=2
                ) as resp:
                    if resp.status == 200:
                        ready = True
                        break
            except Exception:
                pass
            time.sleep(0.1)
        if not ready:
            failures.append(f"sidecar 未就绪（pid={sidecar.pid}）")
        else:
            for scenario in ALL_SCENARIOS:
                _reset_callbacks()
                if bridge_srv is not None:
                    bridge_srv.shutdown()
                    bridge_srv = None

                if scenario["bridge_start"]:
                    # 每个场景重置 mock Bridge 以应用新 scenario
                    _ScenarioContext.set(scenario.get("bridge_scenario", {}))
                    bridge_srv = _start_server(MockBridgeHandler, BRIDGE_PORT)
                    # 给 Bridge 一点时间就绪
                    time.sleep(0.1)

                # 发送 AccessCtrl 请求
                status, body = _http_post(
                    f"{SIDECAR_URL}/coremind/systemagent/v1/callback",
                    _accessctrl(),
                )
                if status != 200:
                    failures.append(
                        f"[{scenario['name']}] sidecar 入口应 200，实际 {status}"
                    )
                if body.strip():
                    failures.append(
                        f"[{scenario['name']}] 异步模式应 200 无 body，"
                        f"实际 body={body[:80]!r}"
                    )

                # 等回调：错误分档只需 TaskStatus（1 个），成功分档需 Completed + TaskStatus（2+）
                min_cb = 1 if not scenario["should_have_completed"] else 2
                n = _wait_callbacks(min_cb, timeout=10.0)
                if n < min_cb:
                    failures.append(
                        f"[{scenario['name']}] 应收到至少 {min_cb} 个回调，"
                        f"实际 {n}（callbacks={json.dumps(_callbacks, ensure_ascii=False)[:200]!r}）"
                    )
                    continue

                # 校验分档预期
                _assert_task_status(
                    failures,
                    scenario["name"],
                    scenario["expected_ts_code"],
                    scenario["expected_ts_status"],
                    scenario["should_have_completed"],
                    scenario["expected_text_prefix"],
                )

                # 分档独立计数
                scenario_failures = [
                    f for f in failures if f.startswith(f"[{scenario['name']}]")
                ]
                if scenario_failures:
                    failed_count += 1
                else:
                    passed_count += 1
    finally:
        sidecar.send_signal(signal.SIGTERM)
        try:
            sidecar.wait(timeout=3)
        except subprocess.TimeoutExpired:
            sidecar.kill()
        if bridge_srv is not None:
            bridge_srv.shutdown()
        cb_srv.shutdown()

    # 全局断言也计入失败（如 sidecar 未就绪）
    global_failures = [
        f for f in failures
        if not any(f.startswith(f"[{s['name']}]") for s in ALL_SCENARIOS)
    ]
    failed_count += len(global_failures)

    # ── 输出（兼容 regression_test.sh 的 run_legacy_e2e_binary）──
    print("")
    for scenario in ALL_SCENARIOS:
        scenario_failures = [
            f for f in failures if f.startswith(f"[{scenario['name']}]")
        ]
        if scenario_failures:
            print(f"  ✗ {scenario['name']}")
            for f in scenario_failures:
                print(f"    → {f}")
        else:
            print(f"  ✓ {scenario['name']}")

    for f in global_failures:
        print(f"  ✗ {f}")

    print("")
    print(f"Tests passed: {passed_count}")
    print(f"Tests failed: {failed_count}")
    return 0 if failed_count == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
