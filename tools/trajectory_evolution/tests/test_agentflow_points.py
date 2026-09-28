# test_agentflow_points 回归验证：检查包内协议、边界条件与失败处理。
# 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
# 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

"""可选宿主回归：真实 v1 函数和采集器，配合固定总线、模型及工具。
需在 AgentFlow 工作区运行，不是独立组件的运行依赖。两个工作进程只传递既有身份。"""

import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import sys
import tempfile
import threading
import time
from unittest.mock import MagicMock, patch

SOURCE = Path(__file__).resolve().parents[1]
REPO = SOURCE.parents[1]
sys.path[:0] = [
    str(SOURCE / "clients/python"),
    str(REPO / "packages/agentflow-runtime/src"),
]


# 测试辅助函数：response。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def response(content="", calls=None):
    message = {"role": "assistant", "content": content}
    if calls:
        message["tool_calls"] = calls
    return {
        "choices": [
            {"message": message, "finish_reason": "tool_calls" if calls else "stop"}
        ],
        "usage": {"prompt_tokens": 10, "completion_tokens": 5},
    }


# 测试辅助函数：tool。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def tool(name, arguments, identity):
    return {
        "id": identity,
        "type": "function",
        "function": {"name": name, "arguments": json.dumps(arguments)},
    }


class FixtureBus:
    # 初始化 __init__ 的状态和资源；输入参数的校验与所有权在构造阶段确定。
    # 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
    def __init__(self, folder):
        self.folder = folder
        folder.mkdir(exist_ok=True)
        self.worker = os.environ.get("AF_EXEC_SKILL_KEY", "")

    # 测试辅助函数：_path。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def _path(self, name):
        return self.folder / hashlib.sha256(name.encode()).hexdigest()

    # 测试辅助函数：_read。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def _read(self, name):
        path = self._path(name)
        return path.read_text() if path.exists() else None

    # 测试辅助函数：_write。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def _write(self, name, value):
        path = self._path(name)
        temporary = path.with_suffix(f".{os.getpid()}.part")
        temporary.write_text(value)
        temporary.replace(path)
        return True

    # 测试辅助函数：is_connected。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def is_connected(self):
        return True

    # 测试辅助函数：get。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def get(self, name):
        return self._read("sys:" + self.worker + ":" + name) or self._read(
            "sys::" + name
        )

    # 测试辅助函数：put。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def put(self, name, value):
        return self._write("sys:" + self.worker + ":" + name, value)

    # 测试辅助函数：get_global。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def get_global(self, name):
        return self._read("global:" + name)

    # 测试辅助函数：put_global。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def put_global(self, name, value):
        return self._write("global:" + name, value)

    # 测试辅助函数：delete。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def delete(self, name):
        self._path("sys:" + self.worker + ":" + name).unlink(missing_ok=True)
        return True

    # 测试辅助函数：response_push。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def response_push(self, *args, **kwargs):
        return True


# 测试辅助函数：node。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def node(folder, mode):
    from agentflow_runtime import (
        coordinator_decide as cd,
        skill_executor as se,
        llm_call,
    )
    from evolution_configured import points

    module = se if mode.startswith("worker") else cd
    bus = FixtureBus(folder / "bus")
    recorder = MagicMock()
    recorder.is_connected = False
    captures, executions = [], []

    # 包装一次真实模型调用，先准备注入请求，再记录原始调用结果。
    def model(messages, *args, **kwargs):
        captures.append(json.loads(json.dumps(messages)))
        if mode == "finalize":
            assert args[3] is None, (
                "the transport reused tools removed by the model point"
            )
            assert "Report failures and unfinished work honestly" in json.dumps(
                messages
            )
            return response("The operation failed; no new work was started.")
        if mode == "dispatch-ordered":
            return response(
                calls=[
                    tool(
                        "run_skills",
                        {
                            "skills": [
                                {
                                    "name": name,
                                    "prompt": "视频生成缺少图片素材，完成 " + name,
                                }
                                for name in ("image", "prepare", "report")
                            ]
                        },
                        "ordered-dispatch",
                    )
                ]
            )
        if mode.startswith("dispatch-repeat"):
            return response(
                calls=[
                    tool(
                        "run_skills",
                        {
                            "skills": [
                                {
                                    "name": "worker-a",
                                    "prompt": "视频生成缺少图片素材，请核验结果。",
                                }
                            ]
                        },
                        "repeat-dispatch",
                    )
                ]
            )
        if mode == "dispatch":
            return response(
                calls=[
                    tool(
                        "run_skills",
                        {
                            "skills": [
                                {
                                    "name": "worker-a",
                                    "prompt": "视频生成缺少图片素材，并行检索 A",
                                },
                                {
                                    "name": "worker-b",
                                    "prompt": "视频生成缺少图片素材，独立尝试 B",
                                },
                            ],
                            "reason": "parallel",
                        },
                        "dispatch-call",
                    )
                ]
            )
        if mode.startswith("worker-ordered"):
            if len(captures) == 1:
                return response(
                    calls=[tool("bash", {"command": "echo " + mode}, "ordered-tool")]
                )
            return response("本分支已完成。")
        if mode.startswith("worker-repeat"):
            if mode == "worker-repeat-recover" and len(captures) == 3:
                assert "当前执行没有产生新进展" in json.dumps(
                    messages, ensure_ascii=False
                ), "engine replan not injected"
                return response("已改变方案并完成任务。")
            command = "echo stable-again" if len(captures) == 3 else "echo stable"
            return response(
                calls=[
                    tool("bash", {"command": command}, "repeat-" + str(len(captures)))
                ]
            )
        if mode.startswith("worker") and len(captures) == 1:
            return response(
                calls=[
                    tool("bash", {"command": "echo slow"}, "call-slow"),
                    tool("bash", {"command": "echo failure"}, "call-failure"),
                ]
                if mode == "worker-a"
                else [tool("video", {}, "call-video")]
            )
        return response("完成，独立任务结果已汇总。")

    recorder.trace_call.side_effect = model
    recorder.record_effective_response.side_effect = lambda _, raw: raw
    registry = MagicMock()
    registry.get_tools.return_value = [
        {
            "type": "function",
            "function": {
                "name": name,
                "parameters": {
                    "type": "object",
                    "properties": {"command": {"type": "string"}},
                },
            },
        }
        for name in ("bash", "video")
    ]
    parallel_entry = threading.Barrier(2, timeout=5) if mode == "worker-a" else None

    # 调用宿主提供的真实执行入口，返回和异常保持原业务语义。
    def execute(name, arguments):
        started = time.monotonic()
        # 通过同步点等待测试入口，不假定调度总在 20 毫秒内完成。
        # 串行派发无法通过该屏障和重叠检查。
        if parallel_entry is not None:
            parallel_entry.wait()
        time.sleep(0.12 if arguments.get("command") == "echo slow" else 0.02)
        executions.append(
            {
                "name": name,
                "arguments": arguments,
                "start": started,
                "end": time.monotonic(),
            }
        )
        return json.dumps(
            {"error": "fixture network failure"}
            if arguments.get("command") == "echo failure"
            else {"stdout": "lookup complete", "exit_code": 0}
        )

    registry.execute.side_effect = execute
    cfg = MagicMock()
    for method, value in {
        "coordinator_max_iterations": 6,
        "max_completion_tokens": 2000,
        "node_timeout_sec": 30,
        "skill_max_iterations": 4,
        "skill_max_completion_tokens": 2000,
        "node_execution_timeout": 30,
        "protect_last_rounds": 0,
        "skill_max_retries": 0,
        "skill_llm_recovery_attempts": 0,
        "fold_dir": "",
        "hard_compression_model": "",
        "fold_compression_enabled": False,
        "hard_compression_enabled": False,
    }.items():
        getattr(cfg, method).return_value = value
    with contextlib.ExitStack() as stack:
        replacements = {
            "DataBusClient": lambda: bus,
            "_load_dotenv": lambda: None,
            "load_hooks_from_dir": lambda *args: None,
            "hook_trigger": lambda _, data, **kwargs: data,
            "emit_event": lambda *args, **kwargs: None,
            "AgentConfig": cfg,
            "parse_agent_md": lambda *args: ({}, "fixture agent"),
            "compress_messages": lambda messages, *args, **kwargs: (messages, "skip"),
            "build_round_reminder": lambda **kwargs: None,
            "resolve_llm_config": lambda *args, **kwargs: {
                "model": "fixture",
                "base_url": "http://127.0.0.1:1",
                "api_key": "fixture-model-credential-never-captured",
            },
            "get_recorder": lambda: recorder,
        }
        if module is cd:
            replacements.update(
                _init_skills=lambda: None, _validate_run_skills_args=lambda *args: ""
            )
        else:
            replacements.update(
                _inject_venv_env=lambda: None,
                _resolve_skill_dir=lambda _: folder,
                load_skills_from_list=lambda _: {},
                _LocalToolRegistry=lambda *args: registry,
                extract_structured_artifacts=lambda *args: [],
                extract_all_artifacts=lambda *args, **kwargs: [],
            )
        for name, value in replacements.items():
            stack.enter_context(patch.object(module, name, value))
        stack.enter_context(patch.object(llm_call, "get_recorder", lambda: recorder))
        output = io.StringIO()
        if mode in ("stop", "worker-stop"):
            # 显式停止提交的宿主处理单独验证，
            # 不与策略引擎的重复状态测试混为一谈。
            stack.enter_context(patch.object(points, "allow_loop", return_value=False))
        with contextlib.redirect_stdout(output):
            if mode == "finalize":
                run_ref = {"run": os.environ["AF_DIALOGUE_ID"]}
                points.point(
                    dict(
                        ref=run_ref,
                        phase="before",
                        kind="run",
                        input="Answer in English",
                    ),
                    user_id="user-a",
                )
                for index in range(3):
                    assert points.allow_loop(
                        dict(
                            ref=dict(run_ref, key=f"loop-{index}"),
                            actor_id="assistant",
                            state="unchanged",
                            loop_id="main",
                            extensions={"finalize_on_stop": True},
                        ),
                        user_id="user-a",
                    )
                actual = llm_call.call_llm(
                    [{"role": "user", "content": "Answer in English"}],
                    tools=[{"type": "function", "function": {"name": "run_skills"}}],
                    source="assistant",
                )
                assert actual["choices"][0]["message"]["content"].startswith(
                    "The operation failed"
                )
                assert not points.allow_loop(
                    dict(
                        ref=dict(run_ref, key="loop-final"),
                        actor_id="assistant",
                        state="changed",
                        loop_id="main",
                        extensions={"finalize_on_stop": True},
                    ),
                    user_id="user-a",
                )
                points.point(
                    dict(ref=run_ref, phase="after", kind="run", output=actual),
                    user_id="user-a",
                )
            else:
                module.execute()
    (folder / (mode + ".result.json")).write_text(
        json.dumps(
            {"models": captures, "tools": executions, "output": output.getvalue()},
            ensure_ascii=False,
        )
    )


# 回归用例：main。
# 重点核验字段或诊断：test_integration_service、max_buffer_bytes、max_no_progress；异常路径不能被当成成功。
def main(build):
    root = Path(tempfile.mkdtemp(prefix="v1-points.", dir=build))
    subprocess.run(
        [str(build / "test_integration_service"), "--fixture", str(root)],
        check=True,
        capture_output=True,
    )
    path = root / "service.json"
    config = json.loads(path.read_text())
    config.update(
        auto_evaluate=False,
        instrumentation={
            "level": "L3",
            "capabilities": ["model", "actions", "schedule", "loop"],
            "binding": {"pointer": "/messages", "mode": "system_message"},
            "output_metrics": {
                "input_tokens": "/usage/prompt_tokens",
                "output_tokens": "/usage/completion_tokens",
            },
        },
        http_persistence={"mode": "buffered", "max_buffer_bytes": 67108864},
    )
    config["runtime_options"]["loop_rules"][0]["max_no_progress"] = 1
    config["runtime_options"]["schedule_rules"] = [
        dict(
            rule_id="prepare-before-image",
            target_scope="agent:my-agent/actor:assistant",
            before_operation="prepare",
            after_operation="image",
            require_success=True,
            reason="准备分支完成后再执行图像分支",
        )
    ]
    path.write_text(json.dumps(config))
    env = {
        **os.environ,
        "INTEGRATION_TEST_A": "fixture-principal-alpha-00000001",
        "INTEGRATION_TEST_B": "fixture-principal-bravo-00000002",
        "AF_SKIP_DOTENV": "1",
        "AF_COORDINATOR_VERSION": "v1",
        "AF_USER_ID": "user-a",
        "AF_CLIENT_ID": "v1-conversation",
        "AF_DIALOGUE_ID": "v1-process-test",
        "AF_SESSION_ID": "0",
        "AF_COMPLETED_SKILL_NAME": "",
        "AF_COMPLETED_BATCH_KEY": "",
        "AF_EXEC_SKILL_KEY": "",
        "AF_DATABUS_UDS_FILE": "",
        "AF_TRACE_OUTPUT_DIR": str(root / "host-traces"),
        "AF_INPUT": "视频生成缺少图片素材，请并发执行并汇总。",
    }
    for name in ("TRAJECTORY_EVOLUTION_TRIAL_URL", "TRAJECTORY_EVOLUTION_TRIAL_TOKEN"):
        env.pop(name, None)
    server = subprocess.Popen(
        [str(build / "trajectory_evolution"), "serve", "--config", str(path)],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(server.stdout, selectors.EVENT_READ)
            assert selector.select(15), "collector startup timeout"
        port = re.search(r"listening=127.0.0.1:(\d+)", server.stdout.readline()).group(
            1
        )
        client_config = root / "client.json"
        client_config.write_text(
            json.dumps(
                {
                    "url": "http://127.0.0.1:" + port,
                    "principals": config["principals"],
                    "point_timeout_ms": 1000,
                    "privacy": {
                        "public_env_names": [
                            "AF_EXEC_SKILL_KEY",
                            "AF_COMPLETED_BATCH_KEY",
                        ]
                    },
                }
            )
        )
        env["EVOLUTION_POINT_CONFIG"] = str(client_config)

        # 测试辅助函数：launch。
        # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
        def launch(mode, extra=None):
            return subprocess.Popen(
                [sys.executable, "-B", __file__, "--node", str(root), mode],
                env={**env, **(extra or {})},
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )

        # 提交实际完成状态；未确认的异步任务不能因为派发函数返回就结束。
        def finish(process):
            out, err = process.communicate(timeout=30)
            assert process.returncode == 0, (out, err)

        # 测试辅助函数：complete。
        # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
        def complete(specs, dialogue):
            results = []
            for spec in specs:
                worker_bus = FixtureBus(root / "bus")
                worker_bus.worker = spec["exec_skill_key"]
                value = json.loads(worker_bus.get("skill_result:skill-runner"))
                results.append(
                    dict(
                        exec_skill_key=spec["exec_skill_key"],
                        business_skill_name=value["skill_name"],
                        workflow_success=True,
                        skill_success=value["status"] in ("completed", "success"),
                        skill_status=value["status"],
                        skill_result=value["skill_result"],
                    )
                )
            key = "completed:" + specs[0]["batch_id"]
            bus.put(
                key, json.dumps(dict(batch_id=specs[0]["batch_id"], results=results))
            )
            finish(
                launch(
                    "complete",
                    {"AF_DIALOGUE_ID": dialogue, "AF_COMPLETED_BATCH_KEY": key},
                )
            )

        finish(launch("dispatch"))
        bus = FixtureBus(root / "bus")
        specs = json.loads(bus.get("next_skills"))
        assert len(specs) == 2
        assert all(
            set(spec) == {"name", "exec_skill_key", "batch_id", "force"}
            for spec in specs
        )
        workers = [
            launch(mode, {"AF_EXEC_SKILL_KEY": spec["exec_skill_key"]})
            for mode, spec in zip(("worker-a", "worker-b"), specs)
        ]
        for worker in reversed(workers):
            finish(worker)
        complete(specs, "v1-process-test")
        traces = root / "data/by-user/user-a/traces/v1-conversation"
        for _ in range(100):
            paths = list(traces.glob("*.trace.json"))
            if paths:
                break
            time.sleep(0.02)
        assert len(paths) == 1, list(root.rglob("*.trace.json"))
        trace = json.loads(paths[0].read_text())
        metrics = trace["attributes"]["metrics"]
        assert metrics["llm_calls"] == 6, metrics
        assert metrics["tool_calls"] == 2, metrics
        assert metrics["tokens"] == 90, metrics
        assert trace["events"][-1]["kind"] == "run.end"
        for mode in ("dispatch", "worker-a", "worker-b", "complete"):
            result = json.loads((root / (mode + ".result.json")).read_text())
            assert "先补充图片" in json.dumps(result["models"], ensure_ascii=False), (
                mode
            )
            if mode == "worker-a":
                assert len(result["tools"]) == 2
                assert max(call["start"] for call in result["tools"]) < min(
                    call["end"] for call in result["tools"]
                )
                second = result["models"][1]
                assert [
                    item["tool_call_id"]
                    for item in second
                    if item.get("role") == "tool"
                ] == [
                    "call-slow",
                    "call-failure",
                ]
            if mode == "worker-b":
                assert result["tools"] == []
                assert "rejected" in json.dumps(result["models"][1])
        doctor = subprocess.run(
            [
                str(build / "trajectory_evolution"),
                "doctor",
                str(paths[0]),
                "--level",
                "L0",
            ],
            capture_output=True,
            text=True,
        )
        assert doctor.returncode == 0, doctor.stderr
        for artifact in (root / "data").rglob("*"):
            if artifact.is_file():
                assert (
                    b"fixture-model-credential-never-captured"
                    not in artifact.read_bytes()
                )
                assert env["INTEGRATION_TEST_A"].encode() not in artifact.read_bytes()
        finish(launch("dispatch-ordered", {"AF_DIALOGUE_ID": "ordered-test"}))
        first_wave = json.loads(bus.get("next_skills"))
        names = lambda wave: [
            json.loads(bus.get_global(item["exec_skill_key"]))["skill_name"]
            for item in wave
        ]
        assert names(first_wave) == ["prepare", "report"], first_wave
        processes = [
            launch(
                "worker-ordered-" + name,
                {
                    "AF_DIALOGUE_ID": "ordered-test",
                    "AF_EXEC_SKILL_KEY": item["exec_skill_key"],
                },
            )
            for item, name in zip(first_wave, names(first_wave))
        ]
        for process in reversed(processes):
            finish(process)
        complete(first_wave, "ordered-test")
        intermediate = json.loads((root / "complete.result.json").read_text())
        assert not intermediate["models"], (
            "a scheduling wave introduced an extra coordinator model call"
        )
        second_wave = json.loads(bus.get("next_skills"))
        assert names(second_wave) == ["image"], second_wave
        finish(
            launch(
                "worker-ordered-image",
                {
                    "AF_DIALOGUE_ID": "ordered-test",
                    "AF_EXEC_SKILL_KEY": second_wave[0]["exec_skill_key"],
                },
            )
        )
        complete(second_wave, "ordered-test")
        ordered_trace = next(
            json.loads(path.read_text())
            for path in traces.glob("*.trace.json")
            if any(
                event.get("attributes", {}).get("receipt", {}).get("boundary")
                == "schedule"
                and event.get("application", {}).get("status") == "applied"
                for event in json.loads(path.read_text())["events"]
            )
        )
        ordered_receipts = [
            event
            for event in ordered_trace["events"]
            if event.get("attributes", {}).get("receipt", {}).get("boundary")
            == "schedule"
            and event.get("application", {}).get("status") == "applied"
        ]
        assert len(ordered_receipts) == 1
        # 通过真实工作线程和模型调用点检验引擎生成的 L3 控制，
        # 不替换循环守卫，也不伪造准备后的模型输入。
        l3 = {}
        for kind in ("recover", "stop"):
            dialogue = "repeat-" + kind
            finish(launch("dispatch-repeat-" + kind, {"AF_DIALOGUE_ID": dialogue}))
            repeated = json.loads(bus.get("next_skills"))[0]
            mode = "worker-repeat-" + kind
            finish(
                launch(
                    mode,
                    {
                        "AF_DIALOGUE_ID": dialogue,
                        "AF_EXEC_SKILL_KEY": repeated["exec_skill_key"],
                    },
                )
            )
            result = json.loads((root / (mode + ".result.json")).read_text())
            assert len(result["models"]) == 3, (
                mode,
                len(result["models"]),
                result["output"],
            )
            assert len(result["tools"]) == (2 if kind == "recover" else 3), (
                mode,
                len(result["tools"]),
                result["output"],
            )
            assert "当前执行没有产生新进展" in json.dumps(
                result["models"][2], ensure_ascii=False
            )
            if kind == "stop":
                assert (
                    json.loads(result["output"].splitlines()[-1])["status"]
                    == "loop_control_stop"
                ), result
            complete([repeated], dialogue)
            l3[kind] = {
                "model_calls": len(result["models"]),
                "tool_calls": len(result["tools"]),
            }
        # 停止处理复用现有终态路径，不额外添加宿主控制器。
        # 引擎生成的停止和重规划由打点回归测试覆盖。
        for mode in ("stop", "worker-stop"):
            finish(
                launch(
                    mode,
                    {
                        "EVOLUTION_POINT_CONFIG": "",
                        "AF_DIALOGUE_ID": mode,
                        "AF_EXEC_SKILL_KEY": specs[0]["exec_skill_key"]
                        if mode == "worker-stop"
                        else "",
                    },
                )
            )
            stopped = json.loads((root / (mode + ".result.json")).read_text())
            assert stopped["models"] == [] and stopped["tools"] == []
            assert (
                json.loads(stopped["output"].splitlines()[-1])["status"]
                == "loop_control_stop"
            )
        finish(
            launch("finalize", {"AF_DIALOGUE_ID": "finalize", "AF_EXEC_SKILL_KEY": ""})
        )
        finalization = json.loads((root / "finalize.result.json").read_text())
        assert len(finalization["models"]) == 1 and finalization["tools"] == []
        unavailable = root / "unavailable-client.json"
        unavailable.write_text(
            json.dumps(
                {
                    "url": "http://127.0.0.1:1",
                    "principals": config["principals"],
                    "point_timeout_ms": 10,
                }
            )
        )
        finish(
            launch(
                "offline",
                {
                    "EVOLUTION_POINT_CONFIG": str(unavailable),
                    "AF_DIALOGUE_ID": "offline",
                },
            )
        )
        offline = json.loads((root / "offline.result.json").read_text())
        assert len(offline["models"]) == 1 and "先补充图片" not in json.dumps(
            offline["models"], ensure_ascii=False
        )
        assert "完成" in offline["output"]
        closed = sorted(traces.glob("*.trace.json"))
        assert len(closed) == 4, closed
        applied = {boundary: 0 for boundary in ("llm", "actions", "schedule", "loop")}
        coordinator_outcomes = {"verified_runs": 0, "failed_skills": 0}
        for trace_path in closed:
            captured = json.loads(trace_path.read_text())
            assert captured["events"][-1]["kind"] == "run.end", trace_path
            doctor = subprocess.run(
                [
                    str(build / "trajectory_evolution"),
                    "doctor",
                    str(trace_path),
                    "--level",
                    "L0",
                ],
                capture_output=True,
                text=True,
            )
            assert doctor.returncode == 0, (trace_path, doctor.stderr)
            # 异步子任务失败必须进入父节点下一次循环查询，
            # 派发确认不能当作已完成的业务动作。
            failures = sum(
                event["kind"] == "action.end"
                and event.get("status") == "failed"
                and event.get("attributes", {}).get("type") == "schedule.action"
                and event.get("loop_id") == "coordinator"
                for event in captured["events"]
            )
            queries = [
                event["attributes"]["decision"]["query"]
                for event in captured["events"]
                if event.get("actor_id") == "coordinator"
                and event.get("attributes", {}).get("type") == "policy.query"
            ]
            queries = [query for query in queries if query.get("boundary") == "loop"]
            assert len(queries) == 2, (trace_path, queries)
            assert int(queries[-1].get("failed_actions", 0)) == failures, (
                trace_path,
                queries[-1],
                failures,
            )
            assert any(
                event.get("attributes", {}).get("delegates_schedule")
                for event in captured["events"]
            ), trace_path
            coordinator_outcomes["verified_runs"] += 1
            coordinator_outcomes["failed_skills"] += failures
            for event in captured["events"]:
                if (
                    event["kind"] == "advice.applied"
                    and event["application"]["status"] == "applied"
                ):
                    applied[event["attributes"]["receipt"]["boundary"]] += 1
        assert all(count > 0 for count in applied.values()), applied
        assert coordinator_outcomes["failed_skills"] == 3, coordinator_outcomes
        for artifact in (root / "data").rglob("*"):
            if artifact.is_file():
                assert (
                    b"fixture-model-credential-never-captured"
                    not in artifact.read_bytes()
                )
                assert env["INTEGRATION_TEST_A"].encode() not in artifact.read_bytes()
        (root / "test-summary.json").write_text(
            json.dumps(
                {
                    "status": "passed",
                    "metrics": metrics,
                    "trace": str(paths[0]),
                    "parallel_workers": 2,
                    "stop_exits": 2,
                    "collector_unavailable_preserved": True,
                    "model_transport": "fixture",
                    "databus_transport": "fixture",
                    "engine_l3": l3,
                    "validated_traces": len(closed),
                    "verified_applications": applied,
                    "coordinator_outcomes": coordinator_outcomes,
                    "engine_l2": {
                        "waves": [names(first_wave), names(second_wave)],
                        "applied": len(ordered_receipts),
                        "intermediate_model_calls": len(intermediate["models"]),
                    },
                },
                indent=2,
            )
        )
        print(root / "test-summary.json")
    finally:
        server.terminate()
        try:
            server.wait(timeout=8)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=3)


if __name__ == "__main__":
    if sys.argv[1] == "--node":
        node(Path(sys.argv[2]), sys.argv[3])
    else:
        main(Path(sys.argv[1]).resolve())
