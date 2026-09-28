# test_instrumentation 回归验证：检查包内协议、边界条件与失败处理。
# 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
# 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

"""验证同一打点信封驱动本地和远程采集，无需 Agent 适配器。"""

import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import sys
import tempfile
import time
import threading
from concurrent.futures import ThreadPoolExecutor
import ctypes
import asyncio
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SOURCE = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv[1]).resolve()
sys.path.insert(0, str(SOURCE / "clients/python"))
from evolution import Evolution
from evolution_local import LocalEvolution

TOKEN = "fixture-principal-alpha-00000001"
os.environ.update(
    INTEGRATION_TEST_A=TOKEN, INTEGRATION_TEST_B="fixture-principal-bravo-00000002"
)


# 测试辅助函数：scenario。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def scenario(client):
    # 提交框架无关打点，必要时在同一边界先取得策略决定，再记录实际执行结果。
    def point(phase, context=None, **fields):
        if context is not None:
            fields["context"] = context
        result = client.invoke("point", dict(fields, phase=phase))
        assert not result.get("degraded"), (phase, fields.get("kind"), result)
        return result

    run = point(
        "before",
        kind="run",
        input="视频生成缺少图片素材",
        conversation_id="point-order",
    )
    root = run["context"]
    control = point("before", root, kind="loop", state="unchanged")
    loop = point("after", control["context"], decision=control["decision"])["context"]
    original = "原始指令，请先检查素材。"
    model = point("before", loop, kind="model", name="any-provider", input=original)
    assert "先补充图片" in model["input"] and original in model["input"]
    end_model = point(
        "after",
        model["context"],
        input=model["input"],
        output="need image",
        executed=True,
        metrics={"input_tokens": 123, "output_tokens": 45},
    )
    first = point("before", loop, kind="action", name="lookup", input={"target": "a"})
    second = point("before", loop, kind="action", name="lookup", input={"target": "b"})
    rejected = point("before", loop, kind="action", name="video", input={})
    assert rejected["decision"] == "reject"
    pending = point("after", root, output="too early")
    assert not pending["closed"] and pending["pending"]
    second_end = point(
        "after",
        second["context"],
        executed=True,
        status="failed",
        output="not available",
    )
    first_end = point("after", first["context"], executed=True, output="result a")
    rejected_end = point(
        "after", rejected["context"], executed=False, output="missing image"
    )
    merged = point(
        "before",
        root,
        kind="custom",
        name="join",
        input="combine",
        causes=[
            end_model["context"],
            first_end["context"],
            second_end["context"],
            rejected_end["context"],
        ],
    )
    point("after", merged["context"], output="partial")
    closed = point("after", root, output="partial", business_status="partial")
    assert closed["closed"] and closed["capture_complete"]
    assert closed["metrics"]["llm_calls"] == 1
    assert closed["metrics"]["tool_calls"] == 2  # 被拒绝动作实际没有执行。
    assert closed["metrics"]["loop_count"] == 1 and closed["metrics"]["tokens"] == 168

    incomplete = point(
        "before", kind="run", input="unknown transport outcome", level="L0"
    )
    prepared = point(
        "before", incomplete["context"], kind="model", name="provider", input="original"
    )
    # 只有输出不能证明增强后的输入真的发出。
    point("after", prepared["context"], output="unverified")
    closed = point("after", incomplete["context"], output="done")
    assert (
        not closed["capture_complete"] and closed["evaluation_status"] == "quarantined"
    )

    declined = point("before", kind="run", input="视频生成缺少图片素材")
    ctx = declined["context"]
    first = point("before", ctx, kind="loop", state="same")
    point("after", first["context"], decision="continue")
    second = point("before", ctx, kind="loop", state="same")
    assert second["decision"] == "replan"
    committed = point("after", second["context"], decision="continue")
    prepared = point(
        "before", committed["context"], kind="model", name="provider", input=original
    )
    assert "停止重复" not in prepared["input"]
    point(
        "after",
        prepared["context"],
        executed=True,
        input=prepared["input"],
        output="next",
    )
    third = point("before", ctx, kind="loop", state="same")
    assert third["decision"] == "replan"
    point("after", third["context"], decision="replan")
    fourth = point("before", ctx, kind="loop", state="same")
    assert fourth["decision"] == "stop"
    continued = point("after", fourth["context"], decision="continue")
    prepared = point(
        "before", continued["context"], kind="action", name="lookup", input={}
    )
    point("after", prepared["context"], executed=True, output="still runs")
    point("after", ctx, output="done")

    failed = point("before", kind="run", input="bad instrumentation", level="L0")
    bad = client.invoke(
        "point",
        {
            "phase": "before",
            "context": failed["context"],
            "kind": "model",
            "input": {"api_key": "dummy-never-persist"},
        },
    )
    assert bad["degraded"] and bad["input"]["api_key"] == "dummy-never-persist"
    final = client.invoke(
        "point",
        {"phase": "after", "kind": "run", "context": bad["context"], "output": "done"},
    )
    assert final["degraded"] and not final["capture_complete"]

    # 非法心跳会标记采集不完整，但不能结束仍在活动的运行。
    live = point("before", kind="run", input="long operation", level="L0")
    heartbeat = client.invoke(
        "point", {"phase": "heartbeat", "context": live["context"], "kind": []}
    )
    assert heartbeat["degraded"] and not heartbeat["closed"]
    status = client.invoke("run.status", {"run_id": live["run_id"]})
    assert not status["closed"] and not status["capture_complete"]
    client.invoke("point", {"phase": "abort", "context": live["context"]})
    malformed = client.invoke(
        "point", {"phase": "after", "context": {"run_id": 123, "kind": []}}
    )
    assert malformed["degraded"] and not malformed["capture_complete"]

    # 未知或降级上下文不能污染无关活动运行。
    unrelated = point("before", kind="run", input="independent capture", level="L0")
    for context in (
        {"run_id": "unobserved-invalid", "kind": "run", "degraded": True},
        {"run_id": "0" * 40, "kind": "custom"},
    ):
        invalid = client.invoke(
            "point", {"phase": "after", "context": context, "output": None}
        )
        assert invalid["degraded"]
    healthy = point("after", unrelated["context"], output="unaffected")
    assert healthy["capture_complete"]

    for referenced in (False, True):
        numeric = {
            "max_tokens": 4096,
            "temperature": 0.25,
            "nested": [0, -3, True, None, 1.5],
        }
        ref = {"run": "numeric-ref"} if referenced else None
        opened = point(
            "before",
            kind="run",
            level="L0",
            input="numeric transport",
            **({"ref": ref} if ref else {}),
        )
        fields = (
            {"ref": dict(ref, key="model")} if ref else {"context": opened["context"]}
        )
        prepared = point("before", kind="model", input=numeric, **fields)
        assert type(prepared["input"]["max_tokens"]) is int
        assert type(prepared["input"]["temperature"]) is float
        assert json.dumps(prepared["input"], sort_keys=True) == json.dumps(
            numeric, sort_keys=True
        )
        point(
            "after",
            input=prepared["input"],
            output="done",
            executed=True,
            **(dict(fields, kind="model") if ref else {"context": prepared["context"]}),
        )
        ended = point(
            "after",
            kind="run",
            output="done",
            **({"ref": ref} if ref else {"context": opened["context"]}),
        )
        assert ended["capture_complete"]

    fallback = client.invoke(
        "point",
        {
            "phase": "before",
            "kind": "model",
            "input": {"max_tokens": 4096},
            "context": {"run_id": "0" * 40, "kind": "run"},
        },
    )
    assert fallback["degraded"] and type(fallback["input"]["max_tokens"]) is int
    return run["run_id"]


# 测试辅助函数：referenced_scenario。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def referenced_scenario(client):
    # 提交框架无关打点，必要时在同一边界先取得策略决定，再记录实际执行结果。
    def point(phase, key=None, **fields):
        ref = dict(run="existing-job-001")
        if key is not None:
            ref["key"] = key
        if "parent" in fields:
            ref["parent"] = fields.pop("parent")
        if "after" in fields:
            ref["after"] = fields.pop("after")
        result = client.invoke("point", dict(fields, phase=phase, ref=ref))
        assert not result.get("degraded"), (phase, key, result)
        return result

    root = point(
        "before", kind="run", input="视频生成缺少图片素材", conversation_id="ref-order"
    )
    assert (
        point(
            "before",
            kind="run",
            input="视频生成缺少图片素材",
            conversation_id="ref-order",
        )
        == root
    )
    control = point("before", "round-1", kind="loop", state="same")
    point("after", "round-1", kind="loop", decision=control["decision"])
    model = point("before", "completion-1", kind="model", input="先检查素材。")
    assert "先补充图片" in model["input"]
    point(
        "after",
        "completion-1",
        kind="model",
        actual_input=model["input"],
        executed=True,
        output="need image",
        metrics={"input_tokens": 20, "output_tokens": 10},
    )
    point("before", "dispatcher", kind="custom", input="schedule")
    for i in range(70):
        point("before", f"worker-{i}", parent="dispatcher", kind="custom", input=i)
    assert point("after", "dispatcher", kind="custom", output="dispatched")["pending"]
    # 派发进程先退出，不要求宿主重试结束或另写结果收集器。
    pending = point("after", kind="run", output="scheduled", business_status="unknown")
    assert pending["pending"] and not pending["closed"]
    for i in reversed(range(70)):
        end = point(
            "after",
            f"worker-{i}",
            kind="custom",
            output=i,
            status="failed" if i == 35 else "completed",
        )
    assert end["run_closed"]
    final = point("after", kind="run", output="scheduled", business_status="unknown")
    assert final["closed"] and final["capture_complete"], final
    assert final["metrics"]["llm_calls"] == 1 and final["metrics"]["tokens"] == 30
    # 回调重试保持相同执行身份，即使传输请求身份更新。
    assert point("after", "worker-1", kind="custom", output=1, status="completed")[
        "ended"
    ]
    return root["run_id"]


# 测试辅助函数：managed_scenario。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def managed_scenario(client, options):
    ref = {"run": "managed-business-call"}

    # 提交框架无关打点，必要时在同一边界先取得策略决定，再记录实际执行结果。
    def point(**fields):
        result = client.point(dict(ref=ref, **fields), **options)
        assert not result.get("degraded"), result
        return result

    point(phase="before", kind="run", input="视频生成缺少图片素材")
    calls = []

    # 包装一次真实模型调用，先准备注入请求，再记录原始调用结果。
    def model(payload):
        calls.append(payload)
        return {"answer": "check image", "usage": {"in": 12, "out": 8}}

    response = client.call(
        dict(ref=dict(ref, key="provider-1"), kind="model", input="先检查素材"),
        model,
        **options,
    )
    assert (
        len(calls) == 1
        and "先补充图片" in calls[0]
        and response["answer"] == "check image"
    )
    blocked = client.call(
        dict(ref=dict(ref, key="tool-1"), kind="action", name="video", input={}),
        model,
        **options,
    )
    assert blocked["status"] == "rejected" and len(calls) == 1

    # 测试辅助函数：asynchronous。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    async def asynchronous():
        # 测试辅助函数：worker。
        # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
        async def worker(payload):
            await asyncio.sleep((3 - payload) * 0.005)
            return payload + 1

        return await asyncio.gather(
            *(
                client.acall(
                    dict(ref=dict(ref, key=f"async-{i}"), kind="custom", input=i),
                    worker,
                    **options,
                )
                for i in range(3)
            )
        )

    assert asyncio.run(asynchronous()) == [1, 2, 3]
    decisions = []
    for i in range(3):
        allowed = client.allow_loop(
            dict(ref=dict(ref, key=f"iteration-{i}"), state="same"), **options
        )
        decisions.append(allowed)
        if allowed:
            client.call(
                dict(ref=dict(ref, key=f"model-{i}"), kind="model", input="先检查素材"),
                model,
                **options,
            )
    assert decisions == [True, True, False], decisions
    done = point(phase="after", kind="run", output="done")
    assert done["capture_complete"] and done["metrics"]["llm_calls"] == 3
    assert done["metrics"]["tokens"] == 60, done
    if isinstance(client, LocalEvolution):
        client.drain()
        assert point(phase="after", kind="run", output="done")["closed"]

    ref = {"run": "managed-exception"}
    point(phase="before", kind="run", level="L0", input="failure isolation")
    failure = ValueError("fixture-secret-must-not-enter-exception-log")

    # 测试辅助函数：fail。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def fail(_):
        raise failure

    try:
        client.call(
            dict(ref=dict(ref, key="failed"), kind="action", input={}), fail, **options
        )
        raise AssertionError("business exception was swallowed")
    except ValueError as error:
        assert error is failure
    assert (
        client.call(
            dict(ref=dict(ref, key="healthy"), kind="custom", input="alive"),
            str,
            **options,
        )
        == "alive"
    )
    done = point(phase="after", kind="run", output="partly done")
    assert done["closed"] and not done["capture_complete"]

    # 本地无法编码的输入只给所属运行标记缺口，不能影响所有运行。
    ref = {"run": "managed-encoding-gap"}
    point(phase="before", kind="run", level="L0", input="gap")
    bad = client.point(
        dict(
            ref=dict(ref, key="unencodable"),
            phase="before",
            kind="custom",
            input=object(),
        ),
        **options,
    )
    assert bad["degraded"]
    done = point(phase="after", kind="run", output="business continued")
    assert done["closed"] and not done["capture_complete"], done

    ref = {"run": "managed-bad-branch-result"}
    opened = point(
        phase="before", kind="run", level="L0", input="parallel capture failure"
    )
    for key in ("bad", "good"):
        client.point(
            dict(ref=dict(ref, key=key), phase="before", kind="custom", input=key),
            **options,
        )
    assert point(phase="after", kind="run", output="scheduled")["pending"]
    bad = client.point(
        dict(
            ref=dict(ref, key="bad"),
            phase="after",
            kind="custom",
            output={"api_key": "dummy-never-persist"},
        ),
        **options,
    )
    assert bad["degraded"]
    good = client.point(
        dict(ref=dict(ref, key="good"), phase="after", kind="custom", output="ok"),
        **options,
    )
    assert not good.get("degraded") and good["run_closed"], good
    status = client.invoke("run.status", {"run_id": opened["context"]["run_id"]})
    assert status["closed"] and not status["capture_complete"], status


# 回归用例：main。
# 重点核验字段或诊断：test_integration_service、max_buffer_bytes；异常路径不能被当成成功。
def main():
    root = Path(tempfile.mkdtemp(prefix="instrumentation.", dir=BUILD))
    for transport in ("native", "http", "http-buffered"):
        folder = root / transport
        subprocess.run(
            [str(BUILD / "test_integration_service"), "--fixture", str(folder)],
            check=True,
            capture_output=True,
        )
        path = folder / "service.json"
        config = json.loads(path.read_text())
        config.update(
            auto_evaluate=False,
            instrumentation={
                "level": "L3",
                "capabilities": ["model", "actions", "loop"],
                "binding": {"pointer": "", "mode": "text"},
                "output_metrics": {
                    "input_tokens": "/usage/in",
                    "output_tokens": "/usage/out",
                },
            },
        )
        if transport == "http-buffered":
            config["http_persistence"] = {
                "mode": "buffered",
                "max_buffer_bytes": 67108864,
            }
        path.write_text(json.dumps(config))
        server = None
        if transport == "native":
            # 压力测试采用文档规定的竞争预算上限；
            # 普通时延和立即降级模式由本地运行时测试覆盖。
            client = LocalEvolution(
                BUILD / "libtrajectory_evolution_local.so",
                path,
                TOKEN,
                contention_budget_us=10000,
            )
        else:
            server = subprocess.Popen(
                [str(BUILD / "trajectory_evolution"), "serve", "--config", str(path)],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
            )
            with selectors.DefaultSelector() as selector:
                selector.register(server.stdout, selectors.EVENT_READ)
                assert selector.select(15), "service startup timeout"
            port = re.search(
                r"listening=127.0.0.1:(\d+)", server.stdout.readline()
            ).group(1)
            client = Evolution("http://127.0.0.1:" + port, TOKEN, retries=0)
        try:
            run_id = scenario(client)
            referenced_scenario(client)
            managed_scenario(
                client, {} if transport == "native" else {"timeout_ms": 1000}
            )
            if transport.startswith("http"):
                damaged = client.point(
                    dict(
                        phase="before",
                        kind="run",
                        level="L0",
                        input="invalid completion",
                    ),
                    timeout_ms=1000,
                )
                left = client.point(
                    dict(
                        phase="before",
                        context=damaged["context"],
                        kind="custom",
                        input="left",
                    ),
                    timeout_ms=1000,
                )
                right = client.point(
                    dict(
                        phase="before",
                        context=damaged["context"],
                        kind="custom",
                        input="right",
                    ),
                    timeout_ms=1000,
                )
                bad = client.point(
                    dict(
                        phase="after",
                        context=left["context"],
                        output={"api_key": "dummy-never-persist"},
                    ),
                    timeout_ms=1000,
                )
                assert bad["degraded"]
                good = client.point(
                    dict(
                        phase="after",
                        context=right["context"],
                        output="right completed",
                    ),
                    timeout_ms=1000,
                )
                assert not good.get("degraded"), good
                done = client.point(
                    dict(
                        phase="after",
                        context=damaged["context"],
                        output="business completed",
                    ),
                    timeout_ms=1000,
                )
                assert (
                    done["closed"]
                    and not done["capture_complete"]
                    and done["evaluation_status"] == "quarantined"
                ), done
            if transport == "native":
                parallel = client.point(
                    dict(
                        phase="before",
                        kind="run",
                        level="L0",
                        input="parallel native points",
                    )
                )
                barrier = threading.Barrier(4, timeout=5)

                # 测试辅助函数：branch。
                # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
                def branch(i):
                    barrier.wait()
                    begun = client.point(
                        dict(
                            phase="before",
                            kind="action",
                            name="worker",
                            input=i,
                            context=parallel["context"],
                        )
                    )
                    barrier.wait()
                    assert not begun.get("degraded"), begun
                    ended = client.point(
                        dict(
                            phase="after",
                            context=begun["context"],
                            executed=True,
                            output=i,
                        )
                    )
                    assert not ended.get("degraded"), ended

                with ThreadPoolExecutor(max_workers=4) as pool:
                    list(pool.map(branch, range(4)))
                done = client.point(
                    dict(phase="after", context=parallel["context"], output="joined")
                )
                assert done["capture_complete"] and done["metrics"]["tool_calls"] == 4

                isolated = client.point(
                    dict(
                        phase="before",
                        kind="run",
                        level="L0",
                        input="invalid concurrent run IDs",
                    )
                )
                context = isolated["context"]
                rid = context["run_id"]
                invalid_rid = rid[:-1] + ("0" if rid[-1] != "0" else "1")
                ready = threading.Barrier(2, timeout=5)

                # 测试辅助函数：invalid_contexts。
                # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
                def invalid_contexts():
                    ready.wait()
                    for _ in range(80):
                        rejected = client.point(
                            dict(
                                phase="after",
                                context={"run_id": invalid_rid, "kind": "custom"},
                                output=None,
                            )
                        )
                        assert rejected["degraded"], rejected

                with ThreadPoolExecutor(max_workers=1) as pool:
                    invalid = pool.submit(invalid_contexts)
                    ready.wait()
                    for i in range(12):
                        begun = client.point(
                            dict(
                                phase="before",
                                kind="custom",
                                context=context,
                                input="负载" * 1200,
                            )
                        )
                        assert not begun.get("degraded"), begun
                        ended = client.point(
                            dict(phase="after", context=begun["context"], output=i)
                        )
                        assert not ended.get("degraded"), ended
                    invalid.result()
                done = client.point(
                    dict(phase="after", context=context, output="isolated")
                )
                assert done["capture_complete"], done
                client.drain()
            trace_path = (
                folder
                / "data/by-user/user-a/traces/point-order"
                / (run_id + ".trace.json")
            )
            # 发布异步进行，不延迟被观测的业务边界。
            for _ in range(100):
                if trace_path.exists():
                    break
                time.sleep(0.02)
            trace = json.loads(trace_path.read_text())
            events = trace["events"]
            starts = [
                e
                for e in events
                if e["kind"] == "step.start"
                and e.get("attributes", {}).get("semantic") == "model"
            ]
            calls = [
                e
                for e in events
                if e["kind"] == "action.start" and e.get("operation") == "any-provider"
            ]
            ends = [
                e
                for e in events
                if e["kind"] == "action.end" and e["action_id"] == calls[0]["action_id"]
            ]
            assert starts[0]["input"] == "原始指令，请先检查素材。"
            assert (
                "先补充图片" in calls[0]["input"]
                and ends[0]["input"] == calls[0]["input"]
            )
            assert (
                events.index(starts[0]) < events.index(calls[0]) < events.index(ends[0])
            )
            assert trace["attributes"]["metrics"]["tokens"] == 168
            checked = subprocess.run(
                [
                    str(BUILD / "trajectory_evolution"),
                    "doctor",
                    str(trace_path),
                    "--level",
                    "L0",
                ],
                capture_output=True,
                text=True,
                timeout=15,
            )
            assert checked.returncode == 0, checked.stderr
        finally:
            if server:
                server.terminate()
                try:
                    server.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=3)
            else:
                client.close()
        for artifact in (folder / "data").rglob("*"):
            if artifact.is_file():
                contents = artifact.read_bytes()
                assert (
                    TOKEN.encode() not in contents
                    and b"dummy-never-persist" not in contents
                )
    print(
        "Native/HTTP points: input order, actual receipts, fan-in, failures, skipped actions, metrics and quarantine passed",
        root,
    )

    class SlowObserver(BaseHTTPRequestHandler):
        attempts = 0

        # 测试辅助函数：log_message。
        # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
        def log_message(self, *_):
            pass

        # 测试辅助函数：do_POST。
        # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
        def do_POST(self):
            self.rfile.read(int(self.headers["Content-Length"]))
            SlowObserver.attempts += 1
            time.sleep(0.15)
            try:
                self.send_response(503)
                self.send_header("Content-Length", "0")
                self.end_headers()
            except OSError:
                pass

    observer = ThreadingHTTPServer(("127.0.0.1", 0), SlowObserver)
    threading.Thread(target=observer.serve_forever, daemon=True).start()
    url = "http://127.0.0.1:" + str(observer.server_port)
    fields = {
        "phase": "before",
        "kind": "model",
        "context": {"run_id": "0" * 40},
        "input": "untouched",
    }
    try:
        began = time.monotonic()
        result = Evolution(url, TOKEN, retries=3).point(fields)
        assert (
            result["degraded"]
            and result["input"] == "untouched"
            and result["decision"] == "allow"
        )
        assert time.monotonic() - began < 0.2
        native = ctypes.CDLL(str(BUILD / "libtrajectory_evolution_local.so"))
        native.te_remote_point.argtypes = [
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.c_uint,
        ]
        native.te_remote_point.restype = ctypes.c_void_p
        native.te_local_free.argtypes = [ctypes.c_void_p]
        body = json.dumps(fields).encode()
        began = time.monotonic()
        pointer = native.te_remote_point(
            url.encode(), TOKEN.encode(), body, len(body), 20
        )
        try:
            result = json.loads(ctypes.string_at(pointer))["result"]
        finally:
            native.te_local_free(pointer)
        assert (
            result["degraded"]
            and result["input"] == "untouched"
            and result["context"]["run_id"] == "0" * 40
        )
        assert time.monotonic() - began < 0.2
        assert SlowObserver.attempts == 2, (
            "point retries must not multiply its latency budget"
        )
    finally:
        observer.shutdown()
        observer.server_close()
    print(
        "Python and C ABI: one bounded HTTP attempt, original input/context preserved on outage"
    )


if __name__ == "__main__":
    main()
