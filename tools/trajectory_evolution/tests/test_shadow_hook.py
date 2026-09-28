# test_shadow_hook 回归验证：检查包内协议、边界条件与失败处理。
# 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
# 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

"""Shadow Hook 回归：真实 C++ 查询、SDK/UDS、跨进程重入和 SessionLog。"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from contextlib import nullcontext
import copy
import importlib.util
import json
import os
from pathlib import Path
import socketserver
import subprocess
import sys
import threading
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[3]
COMPONENT = ROOT / "tools/trajectory_evolution"
HOOK = COMPONENT / "integrations/agentflow/shadow_hook.py"
BINARY = Path(
    os.environ.get(
        "TRAJECTORY_EVOLUTION_EXE",
        ROOT / "build/trajectory-evolution/trajectory_evolution",
    )
)
SDK = ROOT / "src/runtime/databus/clients/python"


# 测试辅助函数：load_module。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# 测试辅助函数：memory_entry。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def memory_entry(name, user="user-a"):
    return {
        "memory_id": name,
        "status": "EVOLUTION_MEMORY_STATUS_CANDIDATE",
        "candidate": {
            "schema_version": 1,
            "candidate_id": "candidate-" + name,
            "generator_version": "scoped-experience-generator-v1",
            "target_type": "CANDIDATE_TARGET_TYPE_EXPERIENCE_INJECTION",
            "target_scope": "coordinator",
            "status": "EVOLUTION_CANDIDATE_STATUS_PROPOSED",
            "trigger_pattern": "生成视频时缺少图片素材",
            "observed_problem": "缺少图片素材仍反复调用工具",
            "proposed_change": "先请求补充图片素材，独立网络检索继续。",
            "support_user_count": 1,
            "support_evidence": [
                {
                    "evidence_id": "evidence-" + name,
                    "source": {"user_id": user, "dialogue_id": "source-" + name},
                }
            ],
        },
    }


# 测试辅助函数：memory_store。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def memory_store():
    return {
        "schema_version": 1,
        "store_kind": "trajectory_evolution_memory",
        "store_version": "evolution-memory-store-v1",
        "revision": "1",
        "entries": [memory_entry("a"), memory_entry("b", "user-b")],
    }


class Namespace:
    # 初始化 __init__ 的状态和资源；输入参数的校验与所有权在构造阶段确定。
    # 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
    def __init__(self):
        self.data, self.writes = {}, 0

    # 测试辅助函数：get。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def get(self, key):
        return self.data.get(key)

    # 测试辅助函数：put。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def put(self, key, value):
        self.writes += 1
        self.data[key] = value
        return True

    # 测试辅助函数：remove。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def remove(self, key):
        return self.data.pop(key, None) is not None


@pytest.fixture
# 测试辅助函数：runtime。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def runtime(monkeypatch, tmp_path):
    assert BINARY.is_file(), (
        "请先构建 trajectory_evolution，或设置 TRAJECTORY_EVOLUTION_EXE"
    )
    from agentflow_runtime.context import AgentContext
    from agentflow_runtime import trace

    module = load_module(HOOK, "shadow_hook_test")
    config = json.loads((COMPONENT / "config/shadow.json").read_text())
    config.update(
        mode="shadow",
        memory_path=str(tmp_path / "memory.json"),
        executable=str(BINARY.resolve()),
    )
    config_path = tmp_path / "shadow.json"

    # 保存当前模块的完整状态，使后台恢复或后续读取能看到一致结果。
    def save():
        config_path.write_text(json.dumps(config, ensure_ascii=False), encoding="utf-8")

    Path(config["memory_path"]).write_text(
        json.dumps(memory_store(), ensure_ascii=False)
    )
    save()
    for key, value in {
        "AF_EVOLUTION_SHADOW_CONFIG": str(config_path),
        "AF_USER_ID": "user-a",
        "AF_CLIENT_ID": "conversation-a",
        "AF_DIALOGUE_ID": "run-a",
        "AF_SESSION_ID": "shared-slot",
        "AF_AGENT_PACKAGE_DIR": str(ROOT / "examples/agent/outfit-assistant"),
        "AF_TRACE_OUTPUT_DIR": str(tmp_path),
        "AF_SESSIONLOG_ENABLED": "1",
        "AF_NODE_ID": "coordinator_decide",
        "AF_SELF_TASK_ID": "coordinator",
    }.items():
        monkeypatch.setenv(key, value)
    for key in ("AF_EXEC_SKILL_KEY", "AF_BATCH_ID"):
        monkeypatch.delenv(key, raising=False)
    monkeypatch.setattr(trace, "current_span_id", lambda: "llm-span-1")
    namespace, records = Namespace(), []
    real_namespace = module._namespace
    monkeypatch.setattr(module, "_namespace", lambda config: nullcontext(namespace))
    monkeypatch.setattr(
        module, "_record", lambda name, data: records.append((name, data))
    )
    ctx = AgentContext(
        messages=[
            {"role": "system", "content": "保持原始策略"},
            {
                "role": "user",
                "content": "生成视频但是没有图片素材，请同时完成独立网络检索",
            },
            {"role": "tool", "tool_call_id": "t1", "content": "生成视频需要图片素材"},
        ]
    )

    # 测试辅助函数：fire。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def fire(name="coordinator_v1.before_llm_call", payload=None):
        module.handle(
            SimpleNamespace(
                name=name,
                payload=payload
                or {
                    "round": 1,
                    "activation_index": 1,
                    "decision_index": 1,
                },
            ),
            ctx,
        )

    return SimpleNamespace(
        module=module,
        config=config,
        save=save,
        fire=fire,
        ctx=ctx,
        namespace=namespace,
        records=records,
        tmp=tmp_path,
        real_namespace=real_namespace,
    )


# 回归用例：test_default_off_has_no_io_or_intervention。
# 关键断言：assert runtime.ctx == original；此处只验证测试构造的场景。
def test_default_off_has_no_io_or_intervention(runtime, monkeypatch):
    monkeypatch.delenv("AF_EVOLUTION_SHADOW_CONFIG")
    monkeypatch.setattr(
        runtime.module,
        "_namespace",
        lambda _: pytest.fail("disabled attempted DataBus"),
    )
    monkeypatch.setattr(
        runtime.module,
        "_invoke",
        lambda *args: pytest.fail("disabled started subprocess"),
    )
    original = copy.deepcopy(runtime.ctx)
    for name in runtime.module.HOOK_EVENTS:
        runtime.fire(name)
    assert runtime.ctx == original
    assert runtime.records == []


@pytest.mark.parametrize(
    "protocol_input",
    [
        "not-json synthetic-credential-do-not-echo",
        '{"memory_path":"synthetic-credential-do-not-echo","unknown_field":true}',
    ],
)
# 回归用例：test_cli_rejects_invalid_protocol_without_echo。
# 重点核验字段或诊断：shadow_request_failed；异常路径不能被当成成功。
def test_cli_rejects_invalid_protocol_without_echo(runtime, protocol_input):
    result = subprocess.run(
        [str(BINARY), "shadow", "compile"],
        input=protocol_input,
        capture_output=True,
        text=True,
        timeout=5,
    )
    assert result.returncode == 2
    assert "synthetic-credential-do-not-echo" not in result.stdout + result.stderr
    assert json.loads(result.stdout) == {
        "status": "error",
        "reason": "shadow_request_failed",
        "mode": "shadow",
        "application_status": "not_applied",
    }


# 回归用例：test_run_snapshot_frozen_and_queries_do_not_change_context。
# 重点核验字段或诊断：max_advice_items；异常路径不能被当成成功。
def test_run_snapshot_frozen_and_queries_do_not_change_context(runtime, monkeypatch):
    original = copy.deepcopy(runtime.ctx)
    runtime.fire("agent.dialogue.start")
    first = runtime.records[-1][1]["snapshot"]["snapshot_id"]
    assert runtime.records[-1][1]["status"] == "ready"
    store = memory_store()
    store["revision"] = "2"
    store["entries"][0]["candidate"]["proposed_change"] = "新经验：先上传图片素材"
    Path(runtime.config["memory_path"]).write_text(json.dumps(store))
    runtime.config["query_scope"] = "changed-scope"
    runtime.config["options"]["max_advice_items"] = 1
    runtime.save()
    runtime.fire("agent.dialogue.start")
    assert runtime.namespace.writes == 1
    assert all(
        key.startswith("run-a/_trajectory_evolution/shadow/")
        for key in runtime.namespace.data
    )
    for _ in range(5):
        runtime.fire()
        result = runtime.records[-1][1]
        assert result["snapshot_id"] == first
        assert result["status"] == "matched"
        assert result["application_status"] == "not_applied"
        assert result["advice"][0]["experience"]["memory_id"] == "a"
    assert (
        len(
            {
                data["query_id"]
                for name, data in runtime.records
                if name == "evolution.shadow.query"
            }
        )
        == 5
    )
    assert runtime.ctx == original
    monkeypatch.setenv("AF_DIALOGUE_ID", "run-b")
    runtime.config["query_scope"] = "coordinator"
    runtime.save()
    runtime.fire("agent.dialogue.start")
    assert runtime.records[-1][1]["snapshot"]["snapshot_id"] != first


@pytest.mark.parametrize(
    "field,value,reason",
    [
        ("AF_USER_ID", "", "identity_invalid"),
        ("AF_AGENT_PACKAGE_DIR", "/agents/another-agent", "agent_binding_mismatch"),
    ],
)
# 回归用例：test_invalid_identity_fails_open。
# 关键断言：assert runtime.records[-1][1]["reason"] == reason；此处只验证测试构造的场景。
def test_invalid_identity_fails_open(runtime, monkeypatch, field, value, reason):
    original = copy.deepcopy(runtime.ctx)
    monkeypatch.setenv(field, value)
    runtime.fire("agent.dialogue.start")
    assert runtime.records[-1][1]["reason"] == reason
    assert not runtime.namespace.data
    assert runtime.ctx == original


# 回归用例：test_no_lazy_compile_and_compile_failure_is_not_retried。
# 重点核验字段或诊断：run_not_initialized；异常路径不能被当成成功。
def test_no_lazy_compile_and_compile_failure_is_not_retried(runtime, monkeypatch):
    original = copy.deepcopy(runtime.ctx)
    runtime.fire()
    assert runtime.records[-1][1]["reason"] == "run_not_initialized"
    attempts = []

    # 测试辅助函数：timeout。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def timeout(*args):
        attempts.append(1)
        raise subprocess.TimeoutExpired("test-component", 0.01)

    monkeypatch.setattr(runtime.module, "_invoke", timeout)
    runtime.fire("agent.dialogue.start")
    for _ in range(3):
        runtime.fire()
        assert runtime.records[-1][1]["reason"] == "component_timeout"
    assert len(attempts) == 1
    assert runtime.ctx == original
    runtime.fire("agent.dialogue.end")
    assert not runtime.namespace.data


# 回归用例：test_one_corrupt_run_does_not_affect_another_user。
# 关键断言：assert runtime.records[-1][1]["status"] == "error"；此处只验证测试构造的场景。
def test_one_corrupt_run_does_not_affect_another_user(runtime, monkeypatch):
    runtime.fire("agent.dialogue.start")
    key = runtime.module._state_key(runtime.module._identity(runtime.config))
    state = json.loads(runtime.namespace.data[key])
    state["snapshot"]["experiences"][0]["content"] = "tampered"
    runtime.namespace.data[key] = json.dumps(state)
    runtime.fire()
    assert runtime.records[-1][1]["status"] == "error"
    monkeypatch.setenv("AF_USER_ID", "user-b")
    runtime.fire("agent.dialogue.start")
    runtime.fire()
    result = runtime.records[-1][1]
    assert result["status"] == "matched"
    assert result["advice"][0]["experience"]["memory_id"] == "b"
    assert "user-a" not in json.dumps(result)


# 回归用例：test_privacy_truncation_and_text_only_context。
# 关键断言：assert result["status"] == "matched"；此处只验证测试构造的场景。
def test_privacy_truncation_and_text_only_context(runtime, monkeypatch):
    secret = "synthetic-shadow-vendor-" + "z" * 24
    monkeypatch.setenv("ANY_VENDOR_TEST_KEY", secret)
    # 非常规变量名沿用现有显式凭据登记接口，不假设任意 *_KEY 都可识别。
    monkeypatch.setenv("AF_SECRET_ENV_NAMES", "ANY_VENDOR_TEST_KEY")
    store = memory_store()
    unsafe = memory_entry("unsafe")
    unsafe["candidate"]["proposed_change"] = "生成视频图片素材 " + secret
    store["entries"].append(unsafe)
    Path(runtime.config["memory_path"]).write_text(json.dumps(store))
    runtime.ctx.messages.append(
        {
            "role": "user",
            "content": [
                {
                    "type": "image_url",
                    "image_url": {"url": "data:image/png;base64,private"},
                },
                {
                    "type": "text",
                    "text": "生成视频图片素材 " + secret + " 长文本" * 10000,
                },
            ],
        }
    )
    original = copy.deepcopy(runtime.ctx)
    runtime.fire("agent.dialogue.start")
    runtime.fire()
    result = runtime.records[-1][1]
    assert result["status"] == "matched"
    assert result["context_truncated"]
    serialized = json.dumps(runtime.records, ensure_ascii=False)
    assert secret not in serialized
    assert "data:image" not in serialized
    assert "保持原始策略" not in serialized
    assert runtime.ctx == original
    task, state, clipped = runtime.module._query_text(runtime.ctx, 4096)
    assert len((task + state).encode()) <= 4096 - 2048
    assert clipped and secret not in task + state


@pytest.mark.parametrize("version", ["v1", "v2"])
# 回归用例：test_real_hook_loader_and_sessionlog_causal_links。
# 重点核验字段或诊断：trajectory_evolution_shadow_v1、source_event_ids；异常路径不能被当成成功。
def test_real_hook_loader_and_sessionlog_causal_links(runtime, monkeypatch, version):
    from agentflow_runtime.hooks import engine
    from agentflow_runtime.hooks.loader import load_hooks_from_dir
    from agentflow_runtime.hooks.registry import HookRegistry
    from agentflow_runtime import hooks, observability

    hooks_dir = runtime.tmp / "hooks"
    hooks_dir.mkdir()
    (hooks_dir / "sessionlog_hook.py").symlink_to(
        ROOT / "docs/sessionlog/sessionlog_hook.py"
    )
    (hooks_dir / "shadow_hook.py").symlink_to(HOOK)
    registry = HookRegistry()
    loaded = load_hooks_from_dir(str(hooks_dir), registry)
    assert "trajectory_evolution_shadow_v1" in loaded
    assert len(loaded) == 2
    handlers = registry.handlers_for("agent.dialogue.start")
    assert handlers[0].__globals__["HOOK_ORDER"] < handlers[1].__globals__["HOOK_ORDER"]
    for handler in handlers:
        data = handler.__globals__
        if data["HOOK_NAME"] == "trajectory_evolution_shadow_v1":
            data["_namespace"] = lambda _: nullcontext(runtime.namespace)
        else:
            data["_get_databus"] = lambda: None
            data["_current_span_id"] = lambda: "llm-span-1"
    monkeypatch.setattr(observability, "emit_event", lambda *args: None)
    monkeypatch.setattr(
        hooks,
        "trigger",
        lambda name, payload: engine.trigger(name, payload, registry=registry),
    )
    original = copy.deepcopy(runtime.ctx)

    # 发送一份已构造的协议请求，保持请求身份、超时和响应上限。
    def send(name, payload):
        prior = copy.deepcopy(payload)
        assert (
            engine.trigger(name, payload, ctx=runtime.ctx, registry=registry) == prior
        )

    send(
        "agent.dialogue.start",
        {"input": "生成视频缺少图片素材", "trigger_type": "first"},
    )
    send("coordinator.trigger", {"trigger_type": "first", "activation_index": 1})
    send(
        "coordinator_" + version + ".before_llm_call",
        {"model": "test-model", "round": 1, "activation_index": 1, "decision_index": 1},
    )
    send(
        "coordinator.llm_result",
        {
            "call_id": "llm-1",
            "skill_name": "coordinator",
            "round": 1,
            "response": {
                "choices": [
                    {
                        "message": {
                            "role": "assistant",
                            "content": "请补充图片素材",
                        },
                        "finish_reason": "stop",
                    }
                ]
            },
        },
    )
    send(
        "agent.dialogue.end", {"status": "completed", "response_text": "请补充图片素材"}
    )
    assert runtime.ctx == original
    assert not runtime.namespace.data
    files = list((runtime.tmp / "sessionlogs/by-user").glob("*/*/run-a.jsonl"))
    assert len(files) == 1
    rows = [json.loads(line) for line in files[0].read_text().splitlines()]
    observation = next(row for row in rows if row["type"] == "evolution.shadow.query")
    request = next(row for row in rows if row["type"] == "llm.request")
    snapshot = next(row for row in rows if row["type"] == "evolution.shadow.snapshot")
    assert observation["data"]["status"] == "matched"
    assert observation["source_event_ids"] == [request["event_id"]]
    assert observation["span_id"] == observation["data"]["decision_id"] == "llm-span-1"
    assert (
        observation["data"]["snapshot_id"]
        == snapshot["data"]["snapshot"]["snapshot_id"]
    )
    assert observation["data"]["application_status"] == "not_applied"
    assert all(row["type"] != "advice.applied" for row in rows)
    assert rows[0]["user_id"] == "user-a"
    assert rows[0]["conversation_id"] == "conversation-a"


@pytest.fixture
# 测试辅助函数：rpc_server。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def rpc_server(tmp_path):
    # 真实 SDK 的 JSON-RPC 协议；仅服务测试内存，不启动 AgentFlow daemon。
    class Handler(socketserver.StreamRequestHandler):
        # 测试辅助函数：handle。
        # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
        def handle(self):
            with self.server.guard:
                self.server.connections += 1
            try:
                for line in self.rfile:
                    request = json.loads(line)
                    params = request["params"]
                    key = (params["session_id"], params["namespace"], params["key"])
                    with self.server.guard:
                        if request["method"] == "put":
                            self.server.data[key] = params["value"]
                            result = {"success": True}
                        elif request["method"] == "get":
                            result = {
                                "exists": key in self.server.data,
                                "value": self.server.data.get(key),
                            }
                        else:
                            result = {
                                "success": self.server.data.pop(key, None) is not None
                            }
                    self.wfile.write(
                        (
                            json.dumps(
                                {
                                    "jsonrpc": "2.0",
                                    "id": request["id"],
                                    "result": result,
                                }
                            )
                            + "\n"
                        ).encode()
                    )
                    self.wfile.flush()
            finally:
                with self.server.guard:
                    self.server.connections -= 1
                    if not self.server.connections:
                        self.server.idle.set()

    class Server(socketserver.ThreadingUnixStreamServer):
        daemon_threads = True

    socket_path = str(tmp_path.parent / "shadow-rpc.sock")
    with Server(socket_path, Handler) as server:
        server.data, server.connections = {}, 0
        server.guard, server.idle = threading.Lock(), threading.Event()
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield server
        finally:
            server.shutdown()
            thread.join(timeout=2)
    Path(socket_path).unlink(missing_ok=True)


# 回归用例：test_sdk_connection_closed_on_return_and_exception。
# 关键断言：assert namespace.put("test-key", "value")；此处只验证测试构造的场景。
def test_sdk_connection_closed_on_return_and_exception(
    runtime, monkeypatch, rpc_server
):
    monkeypatch.syspath_prepend(str(SDK))
    monkeypatch.setenv("AF_DATABUS_UDS_FILE", rpc_server.server_address)
    for fail in (False, True):
        rpc_server.idle.clear()
        try:
            with runtime.real_namespace(runtime.config) as namespace:
                assert namespace.put("test-key", "value")
                assert namespace.get("test-key") == "value"
                if fail:
                    raise ValueError("test failure")
        except ValueError:
            assert fail
        assert rpc_server.idle.wait(1), "UDS 连接未在退出时关闭"
        assert rpc_server.connections == 0


# 回归用例：test_real_subprocess_reentry_two_users_three_conversations。
# 关键断言：assert result.returncode == 0, "Shadow 子进程失败"；此处只验证测试构造的场景。
def test_real_subprocess_reentry_two_users_three_conversations(runtime, rpc_server):
    code = r"""
import importlib.util, json, sys
from agentflow_runtime.context import AgentContext
from agentflow_runtime.events import HookEvent
spec = importlib.util.spec_from_file_location("shadow_child", sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
module._record = lambda name, data: print(json.dumps({"name": name, "data": data}))
request = json.load(sys.stdin)
ctx = AgentContext(messages=[{"role":"user","content":"生成视频但是没有图片素材"}])
for name in request:
    module.handle(HookEvent(name=name, payload={"round":1}), ctx)
"""

    # 测试辅助函数：worker。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def worker(user, conv, names):
        env = dict(
            os.environ,
            AF_USER_ID=user,
            AF_CLIENT_ID=conv,
            AF_DIALOGUE_ID="run-" + conv,
            AF_DATABUS_UDS_FILE=rpc_server.server_address,
            PYTHONPATH=os.pathsep.join(
                (str(SDK), str(ROOT / "packages/agentflow-runtime/src"))
            ),
        )
        result = subprocess.run(
            [sys.executable, "-c", code, str(HOOK)],
            input=json.dumps(names),
            capture_output=True,
            text=True,
            env=env,
            timeout=10,
        )
        assert result.returncode == 0, "Shadow 子进程失败"
        return [json.loads(line)["data"] for line in result.stdout.splitlines()]

    identities = [("user-a", "conv-a1"), ("user-a", "conv-a2"), ("user-b", "conv-b1")]
    start = ["agent.dialogue.start", "coordinator_v1.before_llm_call"]
    with ThreadPoolExecutor(max_workers=3) as pool:
        first = list(pool.map(lambda item: worker(*item, start), identities))
    ids = {items[1]["snapshot_id"] for items in first}
    assert len(ids) == 3
    store = memory_store()
    store["revision"] = "2"
    store["entries"][0]["candidate"]["proposed_change"] = "这条新经验不应进入既有 Run"
    Path(runtime.config["memory_path"]).write_text(json.dumps(store))
    with ThreadPoolExecutor(max_workers=3) as pool:
        second = list(
            pool.map(
                lambda item: worker(
                    *item,
                    ["coordinator_v1.before_llm_call"] * 3 + ["agent.dialogue.end"],
                ),
                identities,
            )
        )
    for identity, earlier, later in zip(identities, first, second):
        for result in later:
            assert result["status"] == "matched"
            assert result["snapshot_id"] == earlier[1]["snapshot_id"]
            assert result["identity"]["user_id"] == identity[0]
            assert result["identity"]["conversation_id"] == identity[1]
            assert result["advice"][0]["experience"]["memory_id"] == (
                "a" if identity[0] == "user-a" else "b"
            )
            assert result["mode"] == "shadow"
            assert result["application_status"] == "not_applied"
    assert not rpc_server.data, "对话结束后残留影子状态"
