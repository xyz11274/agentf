# test_learning_schedule 回归验证：检查包内协议、边界条件与失败处理。
# 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
# 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

"""使用本地固定模型验证增量学习、触发频率和重启恢复。"""

from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import uuid

SOURCE = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv[1]).resolve()
ROOT = Path(tempfile.mkdtemp(prefix="learning-schedule.", dir=BUILD))
TOKENS = {
    "user-a": "fixture-principal-alpha-00000001",
    "user-b": "fixture-principal-bravo-00000002",
}
ENV = dict(
    os.environ, INTEGRATION_TEST_A=TOKENS["user-a"], INTEGRATION_TEST_B=TOKENS["user-b"]
)
os.environ.update(
    INTEGRATION_TEST_A=TOKENS["user-a"], INTEGRATION_TEST_B=TOKENS["user-b"]
)


# 按调用方给出的大小上限读取文件；读取失败向上层报告，不能用空数据冒充成功。
def read(path):
    return json.loads(path.read_text())


# 按照本模块的输出约定写入内容；调用方负责提供已验证的目标和数据。
def write(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False))


# 测试辅助函数：wait。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def wait(predicate, timeout=10):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError("condition timed out; inspect " + str(ROOT))


class Model(BaseHTTPRequestHandler):
    calls = 0
    entered = threading.Event()
    release = threading.Event()
    status = 503
    response = b"fixture model unavailable"
    response_factory = None

    # 测试辅助函数：do_POST。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def do_POST(self):
        raw = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        type(self).calls += 1
        status, response = type(self).status, type(self).response
        if type(self).response_factory is not None:
            status, response = type(self).response_factory(raw)
        type(self).entered.set()
        type(self).release.wait(20)
        try:
            self.send_response(status)
            self.end_headers()
            self.wfile.write(response)
        except (BrokenPipeError, ConnectionResetError):
            pass

    # 测试辅助函数：log_message。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def log_message(self, *args):
        pass


# 测试辅助函数：fixture。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def fixture(name, **learning):
    root = ROOT / name
    subprocess.run(
        [str(BUILD / "test_integration_service"), "--fixture", str(root)],
        env=ENV,
        check=True,
        capture_output=True,
        timeout=20,
    )
    shutil.copytree(SOURCE / "config", root / "config")
    config = read(root / "service.json")
    config.update(
        port=0,
        max_active_runs=2,
        runtime_config=str(root / "config/standard-runtime.json"),
    )
    config["learning"].update(
        min_new_runs=1, max_wait_ms=60000, min_interval_ms=0, retry_backoff_ms=10
    )
    config["learning"].update(learning)
    write(root / "service.json", config)
    runtime = read(root / "config/standard-runtime.json")
    runtime["evaluation"].update(model_selection="all")
    runtime["model"].update(
        endpoint=f"http://127.0.0.1:{MODEL.server_port}/v1/chat/completions",
        model_id="fixture-model",
        no_api_key=True,
        max_retries=0,
        timeout_ms=25000,
    )
    write(root / "config/standard-runtime.json", runtime)
    return root


@contextmanager
# 测试辅助函数：service。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def service(root):
    with (root / "server.stderr").open("a") as errors:
        process = subprocess.Popen(
            [
                str(BUILD / "trajectory_evolution"),
                "serve",
                "--config",
                str(root / "service.json"),
            ],
            env=ENV,
            stdout=subprocess.PIPE,
            stderr=errors,
            text=True,
        )
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                assert selector.select(10), "startup timeout"
            line = process.stdout.readline()
            match = re.search(r"listening=127\.0\.0\.1:(\d+)", line)
            assert match, (line, (root / "server.stderr").read_text())
            endpoint = "http://127.0.0.1:" + match.group(1) + "/v1/invoke"

            # 包装宿主业务调用：先查询策略，再最多执行一次原业务函数，最后记录结果。
            def call(operation, user="user-a", **fields):
                body = dict(fields, operation=operation, request_id=uuid.uuid4().hex)
                request = urllib.request.Request(
                    endpoint,
                    json.dumps(body).encode(),
                    {
                        "Content-Type": "application/json",
                        "Authorization": "Bearer " + TOKENS[user],
                    },
                )
                with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(
                    request, timeout=5
                ) as reply:
                    result = json.load(reply)
                assert result["ok"], result
                return result["result"]

            yield call, process
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(5)


# 测试辅助函数：run。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def run(call, user="user-a", close=True):
    rid = call(
        "run.begin",
        user=user,
        input="检查独立分支与失败回执",
        level="L0",
        conversation_id="conversation",
    )["run_id"]
    activity = call(
        "activity.begin",
        user=user,
        run_id=rid,
        kind="model",
        name="reason",
        input="保持原始请求",
    )
    call(
        "activity.end",
        user=user,
        run_id=rid,
        activity_id=activity["activity_id"],
        status="completed",
        executed=True,
        actual_input=activity["input"],
        output="尝试查询",
    )
    if close:
        call(
            "run.end",
            user=user,
            run_id=rid,
            status="completed",
            business_status="partial",
            output="已完成",
        )
    return rid


# 测试辅助函数：state。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def state(root, rid):
    return read(root / "data/runs" / (rid + ".json"))


# 测试辅助函数：done。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def done(root, ids):
    return all(state(root, rid).get("evaluation_status") == "completed" for rid in ids)


# 测试辅助函数：jobs。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def jobs(root):
    return [read(path) for path in (root / "data/learning/jobs").glob("*.json")]


# 测试辅助函数：report。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def report(root, rid):
    return next(
        (root / "data/by-user").glob("*/evaluation/*/" + rid + ".evaluation.json")
    )


# 测试辅助函数：batching。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def batching():
    before = Model.calls
    root = fixture("count-and-restart", min_new_runs=3)
    with service(root) as (call, _):
        ids = [run(call), run(call)]
        unfinished = run(call, close=False)
        time.sleep(0.15)
        assert Model.calls == before and not jobs(root), (
            "run.begin or a small batch triggered learning"
        )
        call("run.end", run_id=unfinished, status="completed", output="终态")
        ids.append(unfinished)
        wait(lambda: done(root, ids))
        assert Model.calls == before + 3 and len(jobs(root)) == 1
        saved = {
            rid: (report(root, rid).read_bytes(), report(root, rid).stat().st_mtime_ns)
            for rid in ids
        }
        # 已完成但待处理运行多于活动容量时，不能阻塞生产者。
        later = [run(call, "user-b"), run(call, "user-b")]
    with service(root) as (call, _):
        time.sleep(0.15)
        assert Model.calls == before + 3, (
            "restart replayed old reports or flushed the small batch"
        )
        later.append(run(call, "user-b"))
        wait(lambda: done(root, later))
        assert Model.calls == before + 6
    assert all(
        (report(root, rid).read_bytes(), report(root, rid).stat().st_mtime_ns)
        == saved[rid]
        for rid in ids
    )
    assert {job["user"] for job in jobs(root)} == {"user-a", "user-b"}
    assert all(len(job["runs"]) == 3 for job in jobs(root))


# 测试辅助函数：deadlines。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def deadlines():
    root = fixture("max-wait", min_new_runs=3, max_wait_ms=300)
    with service(root) as (call, _):
        rid = run(call)
        wait(lambda: done(root, [rid]))
    root = fixture("cooldown", min_interval_ms=1500)
    with service(root) as (call, _):
        first = run(call)
        wait(lambda: done(root, [first]))
        second = run(call)
    before = Model.calls
    with service(root):
        assert state(root, second)["evaluation_status"] == "pending", (
            "restart lost cooldown"
        )
        time.sleep(0.15)
        assert Model.calls == before
        wait(lambda: done(root, [second]))


# 测试辅助函数：configuration_backoff。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def configuration_backoff():
    root = fixture("configuration-backoff", retry_backoff_ms=3000)
    path = root / "config/standard-runtime.json"
    good = read(path)
    invalid = dict(good, schema_version=999)
    before = Model.calls
    schedule = root / "data/learning/schedule.json"
    with service(root) as (call, _):
        write(path, invalid)
        rid = run(call)
        wait(
            lambda: (
                schedule.exists()
                and read(schedule).get("last_error")
                == "evaluation_configuration_failed"
            )
        )
        assert state(root, rid)["evaluation_status"] == "pending" and not jobs(root)
        saved_time = schedule.stat().st_mtime_ns
    write(path, good)
    with service(root):
        time.sleep(0.15)
        assert Model.calls == before and schedule.stat().st_mtime_ns == saved_time, (
            "restart discarded storage/configuration backoff"
        )
        wait(lambda: done(root, [rid]))
        assert Model.calls == before + 1


# 测试辅助函数：busy_arrivals。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def busy_arrivals():
    root = fixture("busy", min_new_runs=2)
    Model.entered.clear()
    Model.release.clear()
    try:
        with service(root) as (call, _):
            first = [run(call), run(call)]
            assert Model.entered.wait(5)
            frozen = jobs(root)[0]
            assert set(frozen["runs"]) == set(first)
            start = time.monotonic()
            later = [run(call, "user-b"), run(call, "user-b")]
            assert time.monotonic() - start < 2, "background model delayed host capture"
            assert len(jobs(root)) == 1 and jobs(root)[0]["runs"] == frozen["runs"]
            Model.release.set()
            wait(lambda: done(root, first + later))
            assert len(jobs(root)) == 2
    finally:
        Model.release.set()


# 测试辅助函数：interrupted_analysis。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def interrupted_analysis():
    root = fixture("interrupted-analysis")
    Model.entered.clear()
    Model.release.clear()
    before = Model.calls
    try:
        with service(root) as (call, process):
            rid = run(call)
            assert Model.entered.wait(5)
            process.kill()
            process.wait(5)
    finally:
        Model.release.set()
    with service(root) as (call, _):
        wait(lambda: state(root, rid)["evaluation_status"] == "failed")
        assert (
            state(root, rid)["evaluation_error"]
            == "analysis_interrupted_unknown_outcome"
        )
        assert Model.calls == before + 1, (
            "ambiguous crashed request was automatically rebilled"
        )
        call("evaluation.retry", run_id=rid)
        wait(lambda: done(root, [rid]))
        assert Model.calls == before + 2


# 测试辅助函数：strict_error_codes。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def strict_error_codes():
    # 测试辅助函数：completion。
    # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
    def completion(content, finish="stop"):
        return json.dumps(
            {"choices": [{"message": {"content": content}, "finish_reason": finish}]}
        ).encode()

    cases = [
        (503, TOKENS["user-a"].encode(), "http_503"),
        (200, completion("{not-json"), "invalid_json"),
        (200, completion("{", "length"), "output_token_limit_reached"),
        (200, completion(TOKENS["user-a"]), "credential_like_output"),
        (503, b"fixture timeout", "transport_28"),
    ]
    try:
        for status, response, code in cases:
            Model.status, Model.response = status, response
            root = fixture("strict-" + code)
            runtime = read(root / "config/standard-runtime.json")
            runtime["evaluation"]["strict_model"] = True
            runtime["model"].update(api_key_env="INTEGRATION_TEST_A", no_api_key=False)
            if code == "transport_28":
                # 只让本地模型桩延迟，快速重现真实请求超时，不使用远程 API。
                runtime["model"]["timeout_ms"] = 200
                Model.release.clear()
            write(root / "config/standard-runtime.json", runtime)
            before = Model.calls
            with service(root) as (call, _):
                rid = run(call)
                wait(lambda: state(root, rid)["evaluation_status"] == "failed")
                Model.release.set()
                expected = "dialogue_model_" + code
                assert state(root, rid)["evaluation_error"] == expected, state(
                    root, rid
                )
                assert jobs(root)[0]["errors"][rid] == expected
                assert state(root, rid)["learning"]["analysis_state"] == "failed"
                assert Model.calls == before + 1, (
                    "strict failure must not add unbounded retries"
                )
                assert not list(
                    (root / "data/by-user").glob("*/evaluation/*/*.evaluation.json")
                )
            if code == "http_503":
                # 模拟旧版本遗留记录：恢复时修复内部终态，但不能自动重发
                # 可能已计费的请求，也不能把失败运行标成成功。
                previous = state(root, rid)
                previous["learning"]["analysis_state"] = "running"
                write(root / "data/runs" / (rid + ".json"), previous)
                with service(root):
                    assert state(root, rid)["evaluation_status"] == "failed"
                    assert state(root, rid)["learning"]["analysis_state"] == "failed"
                    assert state(root, rid)["evaluation_error"] == expected
                    assert Model.calls == before + 1
    finally:
        Model.release.set()
        Model.status, Model.response = 503, b"fixture model unavailable"


# 测试辅助函数：checkpoints。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def checkpoints():
    root = fixture("reuse-analysis")
    before = Model.calls
    memory = root / "data/by-user/user-a/memory/evolution-memory.pb"
    good_memory = memory.read_bytes()
    with service(root) as (call, _):
        memory.write_bytes(b"invalid fixture memory")
        rid = run(call)
        wait(lambda: state(root, rid)["evaluation_status"] == "failed")
        saved = report(root, rid).read_bytes()
        assert state(root, rid)["learning"]["analysis_state"] == "completed"
        memory.write_bytes(good_memory)
        call("evaluation.retry", run_id=rid)
        wait(lambda: done(root, [rid]))
        assert Model.calls == before + 1 and report(root, rid).read_bytes() == saved
    job = jobs(root)[0]
    candidate = root / "data/learning/jobs" / (job["id"] + ".candidates.pb")
    candidate_time = candidate.stat().st_mtime_ns
    revision = read(root / "data/by-user/user-a/memory/evolution-memory.json")[
        "revision"
    ]
    # 模拟记忆提交后、逐运行完成标记之前进程崩溃。
    job.update(state="running", phase="memory_curation")
    write(root / "data/learning/jobs" / (job["id"] + ".json"), job)
    value = state(root, rid)
    value["evaluation_status"] = "running"
    write(root / "data/runs" / (rid + ".json"), value)
    with service(root):
        wait(lambda: done(root, [rid]))
        assert (
            Model.calls == before + 1 and candidate.stat().st_mtime_ns == candidate_time
        )
        assert (
            read(root / "data/by-user/user-a/memory/evolution-memory.json")["revision"]
            == revision
        )
    # 操作者删除已完成报告，不能触发静默重新分析。
    report(root, rid).unlink()
    job.update(state="running", phase="memory_curation")
    write(root / "data/learning/jobs" / (job["id"] + ".json"), job)
    value = state(root, rid)
    value["evaluation_status"] = "running"
    write(root / "data/runs" / (rid + ".json"), value)
    with service(root):
        wait(lambda: state(root, rid)["evaluation_status"] == "failed")
        assert state(root, rid)["evaluation_error"] == "analysis_cache_missing"
        assert Model.calls == before + 1


# 测试辅助函数：isolated_failure。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def isolated_failure():
    root = fixture("isolated-failure", min_new_runs=2)
    before = Model.calls
    with service(root) as (call, _):
        first = run(call)
        wait(lambda: state(root, first)["learning"].get("trace_published"))
        path = (
            root
            / "data/by-user/user-a/evaluation/conversation"
            / (first + ".evaluation.json")
        )
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("not valid JSON")
        second = run(call)
        wait(
            lambda: (
                state(root, first)["evaluation_status"] == "failed"
                and done(root, [second])
            )
        )
        assert Model.calls == before + 1, "bad cache must not be silently overwritten"
        assert len(jobs(root)[0]["errors"]) == 1


# 测试辅助函数：publication_failure。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def publication_failure():
    root = fixture("publication-failure")
    blocked = root / "data/by-user/user-a/traces"
    blocked.write_text("fixture obstruction")
    with service(root) as (call, _):
        first = run(call)
        wait(lambda: state(root, first)["evaluation_status"] == "failed")
        assert (
            state(root, first)["learning"]["publication_error"]
            == "trace_publication_failed"
        )
        second = run(call, "user-b")
        wait(lambda: done(root, [second]))
        blocked.rename(blocked.with_name("traces.obstruction"))
        call("evaluation.retry", run_id=first)
        wait(lambda: done(root, [first]))


# 测试辅助函数：bounded_batches。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def bounded_batches():
    root = fixture("model-budget", min_new_runs=3, max_wait_ms=300, max_batch_runs=3)
    path = root / "config/standard-runtime.json"
    config = read(path)
    config["evaluation"]["max_model_calls"] = 2
    write(path, config)
    before = Model.calls
    with service(root) as (call, _):
        ids = [run(call) for _ in range(3)]
        wait(lambda: done(root, ids))
    assert Model.calls == before + 3 and sorted(
        len(job["runs"]) for job in jobs(root)
    ) == [1, 2]
    assert all(sum(job["analysis_model_calls"].values()) <= 2 for job in jobs(root))


# 测试辅助函数：idle_cache。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def idle_cache():
    root = fixture("idle-cache", min_new_runs=3)
    spec = importlib.util.spec_from_file_location(
        "local", SOURCE / "clients/python/evolution_local.py"
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    client = module.LocalEvolution(
        BUILD / "libtrajectory_evolution_local.so",
        root / "service.json",
        TOKENS["user-a"],
    )
    try:
        initial = client.metrics()["memory_refreshes"]
        time.sleep(0.7)
        assert client.metrics()["memory_refreshes"] == initial == 1, (
            "idle cache was reread"
        )
        rid = client.invoke(
            "run.begin", {"input": "只收集，不触发学习", "level": "L0"}
        )["run_id"]
        client.invoke(
            "run.end", {"run_id": rid, "status": "completed", "output": "完成"}
        )
        wait(lambda: client.invoke("run.status", {"run_id": rid})["persisted"])
        time.sleep(0.15)
        assert client.metrics()["memory_refreshes"] == initial and not jobs(root)
        for _ in range(2):
            rid = client.invoke("run.begin", {"input": "新的完整样本", "level": "L0"})[
                "run_id"
            ]
            client.invoke(
                "run.end", {"run_id": rid, "status": "completed", "output": "完成"}
            )
            wait(lambda: client.invoke("run.status", {"run_id": rid})["persisted"])
        wait(lambda: client.metrics()["memory_refreshes"] == initial + 1)
        time.sleep(0.2)
        assert client.metrics()["memory_refreshes"] == initial + 1
    finally:
        client.close()


# 测试辅助函数：archived_jobs_restart。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def archived_jobs_restart():
    root = fixture("archived-jobs-restart")
    config = read(root / "service.json")
    config["max_stored_runs"] = 2
    write(root / "service.json", config)
    before = Model.calls
    with service(root) as (call, _):
        for _ in range(4):
            rid = run(call)
            wait(lambda: done(root, [rid]))
        assert len(jobs(root)) == 4 and Model.calls == before + 4
    saved = {
        path: path.stat().st_mtime_ns
        for path in (root / "data/by-user").rglob("*.evaluation.json")
    }
    with service(root):
        time.sleep(0.15)
        assert Model.calls == before + 4, "restart replayed archived learning jobs"
        assert all(job["state"] == "completed" for job in jobs(root))
    assert all(
        path.stat().st_mtime_ns == timestamp for path, timestamp in saved.items()
    )


# 测试辅助函数：repeated_shutdown_drains。
# 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
def repeated_shutdown_drains():
    root = fixture("repeated-shutdown")
    Model.entered.clear()
    Model.release.clear()
    try:
        with service(root) as (call, process):
            rid = run(call)
            assert Model.entered.wait(5)
            process.terminate()
            time.sleep(0.4)
            process.terminate()
            time.sleep(0.15)
            assert process.poll() is None, (
                "second SIGTERM interrupted durable worker drain"
            )
            Model.release.set()
            assert process.wait(10) == 0
            assert done(root, [rid])
        before = Model.calls
        with service(root):
            time.sleep(0.15)
            assert Model.calls == before, "drained shutdown caused duplicate analysis"
    finally:
        Model.release.set()


def retry_fixture(name, **learning):
    """重试回归仅开启严格轨迹分析，避免反思或公共提炼干扰请求计数。"""
    root = fixture(name, max_analysis_retries=2, **learning)
    config = read(root / "service.json")
    config["sharing"] = {"enabled": False}
    write(root / "service.json", config)
    path = root / "config/standard-runtime.json"
    runtime = read(path)
    runtime["evaluation"]["strict_model"] = True
    runtime["evolution"].update(enable_meta_reflection=False, enable_memory_semantic_matching=False)
    write(path, runtime)
    return root


def analysis_reply(raw):
    """用输入中的实际事件构造协议正确的本地分析响应，不调用付费模型。"""
    text = json.loads(raw)["messages"][-1]["content"]
    payload, _ = json.JSONDecoder().raw_decode(text.split("输入证据：\n", 1)[1])
    events = [e["event_id"] for e in payload["content_events"] if "event_id" in e][:1]
    response = dict(
        task_outcome=dict(status="unknown", confidence=0.9, reason="仅验证处理链路。",
                          evidence_event_ids=events),
        episode_assessments=[dict(episode_id=episode, verdict="uncertain", confidence=0.9,
                                 concise_reason="固定测试响应不推断业务效果。",
                                 evidence_event_ids=events)
                             for episode in payload["selected_episode_ids"]],
        reflection=dict(summary="仅验证自动重试。", root_causes=[], skill_orchestration_changes=[],
                        experience_injections=[], stop_condition_changes=[], wasted_episode_ids=[],
                        evidence_event_ids=events),
    )
    return 200, json.dumps({"choices": [{"message": {"content": json.dumps(response)},
                                         "finish_reason": "stop"}]}).encode()


def automatic_retry_restart_and_cli():
    """同批只补失败轮；重启保留退避，离线 learn 等待期限而非空转退出。"""
    root = retry_fixture("automatic-retry-restart", min_new_runs=2, retry_backoff_ms=1800)
    before = Model.calls
    Model.response_factory = lambda raw: ((503, b"temporary") if Model.calls == before + 1
                                          else analysis_reply(raw))
    try:
        with service(root) as (call, _):
            ids = [run(call), run(call)]
            wait(lambda: any(state(root, rid)["learning"].get("analysis_retry_count") == 1
                             for rid in ids))
            retry = next(rid for rid in ids if state(root, rid)["evaluation_status"] == "pending")
            other = next(rid for rid in ids if rid != retry)
            wait(lambda: done(root, [other]))
            saved = report(root, other).read_bytes(), report(root, other).stat().st_mtime_ns
            learning = state(root, retry)["learning"]
            assert learning["last_analysis_error"] == "dialogue_model_http_503"
            assert learning["analysis_state"] == "retry_authorized"
            assert Model.calls == before + 2
        # 服务退出后只用原生离线入口恢复，不另写队列记录或调用重试接口。
        result = subprocess.run([str(BUILD / "trajectory_evolution"), "learn", "--config",
                                 str(root / "service.json"), "--max-cycles", "8"],
                                env=ENV, capture_output=True, timeout=15)
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert done(root, ids) and Model.calls == before + 3
        assert state(root, retry)["learning"]["analysis_retry_count"] == 1
        assert time.time() * 1000 >= learning["retry_after_ms"]
        assert saved == (report(root, other).read_bytes(), report(root, other).stat().st_mtime_ns)
    finally:
        Model.response_factory = None


def automatic_retry_limits():
    """暂时错误最多补两次；永久错误不补跑，重启也不能重置用尽的预算。"""
    for status, response, expected_calls in ((503, b"temporary", 3), (401, b"denied", 1),
                                            (200, b'{"choices":[]}', 1)):
        root = retry_fixture("retry-limit-" + str(status), retry_backoff_ms=20)
        before = Model.calls
        Model.status, Model.response = status, response
        try:
            with service(root) as (call, _):
                rid = run(call)
                wait(lambda: state(root, rid)["evaluation_status"] == "failed")
                assert Model.calls == before + expected_calls
                assert state(root, rid)["learning"]["analysis_state"] == "failed"
                assert state(root, rid)["learning"].get("analysis_retry_count", 0) == expected_calls - 1
            with service(root):
                time.sleep(0.1)
                assert Model.calls == before + expected_calls
                assert state(root, rid)["evaluation_status"] == "failed"
        finally:
            Model.status, Model.response = 503, b"fixture model unavailable"


def automatic_retry_local_runtime():
    """原生客户端使用相同后台重试，宿主只打点，不编写额外重试器。"""
    root = retry_fixture("automatic-retry-local", retry_backoff_ms=30)
    spec = importlib.util.spec_from_file_location("retry_local", SOURCE / "clients/python/evolution_local.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    before = Model.calls
    Model.response_factory = lambda raw: ((503, b"temporary") if Model.calls == before + 1
                                          else analysis_reply(raw))
    client = None
    try:
        client = module.LocalEvolution(BUILD / "libtrajectory_evolution_local.so",
                                       root / "service.json", TOKENS["user-a"])

        def call(operation, user="user-a", **fields):
            """将现有采集辅助函数接到原生入口，不改原业务调用。"""
            assert user == "user-a"
            return client.invoke(operation, fields)

        rid = run(call)
        # 原生采集异步落盘；先等待持久化回执，再核对后台学习文件。
        wait(lambda: client.invoke("run.status", {"run_id": rid})["persisted"])
        wait(lambda: done(root, [rid]))
        assert Model.calls == before + 2
        assert state(root, rid)["learning"]["analysis_retry_count"] == 1
    finally:
        if client is not None:
            client.close()
        Model.response_factory = None


def automatic_task_outcome_retry(error="invalid_task_outcome"):
    """模型协议错误只补失败轮；持续无效时耗尽预算，重启不能伪造成功或再次计费。"""
    for permanent in (False, True):
        root = retry_fixture(error + "-retry-" + str(permanent), retry_backoff_ms=20)
        before = Model.calls

        def reply(raw):
            status, body = analysis_reply(raw)
            if permanent or Model.calls == before + 1:
                envelope = json.loads(body)
                content = json.loads(envelope["choices"][0]["message"]["content"])
                # 只破坏指定的契约，成功重试仍必须通过完整的证据与结构校验。
                if error == "invalid_task_outcome":
                    content["task_outcome"]["status"] = "not-an-outcome"
                elif error == "episode_assessment_count_mismatch":
                    content["episode_assessments"].append({})
                elif error == "missing_dialogue_section":
                    content.pop("reflection")
                elif error == "invalid_episode_assessment":
                    content["episode_assessments"][0] = False
                elif error == "unknown_or_duplicate_episode_id":
                    content["episode_assessments"][0]["episode_id"] = "absent-episode"
                elif error.startswith("invalid_episode_"):
                    field, invalid = {
                        "invalid_episode_verdict": ("verdict", "success"),
                        "invalid_episode_confidence": ("confidence", 2.0),
                        "invalid_episode_state_change": ("expected_state_change", []),
                        "invalid_episode_concise_reason": ("concise_reason", ""),
                        "invalid_episode_evidence_event_ids": ("evidence_event_ids", ["absent-event"]),
                    }[error]
                    content["episode_assessments"][0][field] = invalid
                elif error.startswith("invalid_reflection_"):
                    field, invalid = {
                        "invalid_reflection_summary": ("summary", ""),
                        "invalid_reflection_root_causes": ("root_causes", None),
                        "invalid_reflection_wasted_episode_ids": ("wasted_episode_ids", None),
                        "invalid_reflection_evidence_event_ids": ("evidence_event_ids", ["absent-event"]),
                    }[error]
                    content["reflection"][field] = invalid
                elif error == "unknown_wasted_episode_id":
                    content["reflection"]["wasted_episode_ids"] = ["absent-episode"]
                else:
                    assert error == "invalid_json", error
                envelope["choices"][0]["message"]["content"] = (
                    "{not-json" if error == "invalid_json" else json.dumps(content))
                body = json.dumps(envelope).encode()
            return status, body

        Model.response_factory = reply
        try:
            with service(root) as (call, _):
                rid = run(call)
                expected = "failed" if permanent else "completed"
                wait(lambda: state(root, rid)["evaluation_status"] == expected)
                current = state(root, rid)
                assert Model.calls == before + (3 if permanent else 2)
                assert current["learning"]["analysis_retry_count"] == (2 if permanent else 1)
                assert current["learning"]["last_analysis_error"] == "dialogue_model_" + error
                if permanent:
                    assert not any((root / "data").rglob(rid + ".evaluation.json"))
                    assert current["evaluation_error"] == "dialogue_model_" + error
                else:
                    assert report(root, rid).exists()
                    assert not current.get("evaluation_error")
            # 重启必须复用成功报告，或保留已用尽的预算，不重复请求。
            saved_calls = Model.calls
            with service(root):
                time.sleep(0.15)
                assert Model.calls == saved_calls
        finally:
            Model.response_factory = None


def automatic_episode_count_retry():
    """模型漏评或多评条目时按同一持久预算恢复，重启不重新请求已成功的轮次。"""
    automatic_task_outcome_retry("episode_assessment_count_mismatch")


def automatic_dialogue_protocol_retry():
    """覆盖当前严格校验器的其余响应协议错误，不等真实运行逐个暴露才加入名单。"""
    for error in (
        "invalid_json", "missing_dialogue_section", "invalid_episode_assessment",
        "unknown_or_duplicate_episode_id", "invalid_episode_verdict", "invalid_episode_confidence",
        "invalid_episode_state_change", "invalid_episode_concise_reason", "invalid_episode_evidence_event_ids",
        "invalid_reflection_summary", "invalid_reflection_root_causes", "invalid_reflection_wasted_episode_ids",
        "invalid_reflection_evidence_event_ids", "unknown_wasted_episode_id",
    ):
        automatic_task_outcome_retry(error)


def protocol_retry_boundaries():
    """合法业务失败、隐私和输出限额不触发协议补跑；只重试坏响应，不能重试到结论好看。"""
    for outcome in ("business_failed", "credential_like_output", "output_token_limit_reached"):
        root = retry_fixture("protocol-boundary-" + outcome, retry_backoff_ms=20)
        before = Model.calls

        def reply(raw):
            status, body = analysis_reply(raw)
            envelope = json.loads(body)
            choice = envelope["choices"][0]
            content = json.loads(choice["message"]["content"])
            if outcome == "business_failed":
                content["task_outcome"]["status"] = "failed"
                for episode in content["episode_assessments"]:
                    episode["verdict"] = "invalid"
                choice["message"]["content"] = json.dumps(content)
            elif outcome == "credential_like_output":
                choice["message"]["content"] = "sk-" + "x" * 40
            else:
                choice["finish_reason"] = "length"
            return status, json.dumps(envelope).encode()

        Model.response_factory = reply
        try:
            with service(root) as (call, _):
                rid = run(call)
                expected = "completed" if outcome == "business_failed" else "failed"
                wait(lambda: state(root, rid)["evaluation_status"] == expected)
                assert Model.calls == before + 1
                assert state(root, rid)["learning"].get("analysis_retry_count", 0) == 0
                if outcome == "business_failed":
                    assert read(report(root, rid))["dialogue_assessment"]["task_outcome"] == "AGGREGATE_STATUS_FAILED"
                else:
                    assert state(root, rid)["evaluation_error"] == "dialogue_model_" + outcome
            with service(root):
                time.sleep(0.1)
                assert Model.calls == before + 1
        finally:
            Model.response_factory = None


def parallel_analysis():
    """慢分支不占住全部队列；乱序结果仍按冻结顺序合成，失败只隔离一轮。"""
    for failed_branch in (False, True):
        root = retry_fixture("parallel-analysis-" + str(failed_branch),
                             min_new_runs=6, max_batch_runs=6, analysis_parallelism=3)
        config = read(root / "service.json")
        config["learning"]["max_analysis_retries"] = 0
        write(root / "service.json", config)
        counters = dict(arrivals=0, active=0, peak=0)
        lock = threading.Lock()
        start = threading.Event()
        slow = threading.Event()
        before = Model.calls

        def reply(raw):
            # 第一个请求持续阻塞，其他请求须能完成并补入剩余三轮。
            with lock:
                counters["arrivals"] += 1
                ordinal = counters["arrivals"]
                counters["active"] += 1
                counters["peak"] = max(counters["peak"], counters["active"])
            try:
                assert start.wait(8), "parallel requests never filled the available slots"
                if ordinal == 1:
                    assert slow.wait(8), "slow branch was not released"
                if failed_branch and ordinal == 2:
                    return 503, b"isolated failure"
                return analysis_reply(raw)
            finally:
                with lock:
                    counters["active"] -= 1

        Model.response_factory = reply
        try:
            with service(root) as (call, _):
                ids = [run(call) for _ in range(6)]
                wait(lambda: counters["arrivals"] == 3)
                time.sleep(0.05)
                assert counters["arrivals"] == 3 and counters["peak"] == 3
                start.set()
                wait(lambda: counters["arrivals"] == 6)
                expected_ready = 4 if failed_branch else 5
                wait(lambda: len(list((root / "data/by-user").rglob("*.evaluation.json")))
                     == expected_ready)
                assert counters["active"] == 1, "a slow request blocked independent results"
                slow.set()
                wait(lambda: all(state(root, rid)["evaluation_status"] in ("completed", "failed")
                                 for rid in ids))
                assert Model.calls == before + 6
                assert sum(state(root, rid)["evaluation_status"] == "failed" for rid in ids) == int(failed_branch)
                job = jobs(root)[0]
                assert len(job["errors"]) == int(failed_branch)
                assert all(code == "dialogue_model_http_503" for code in job["errors"].values())
                material = ""
                saved = {}
                for rid in job["runs"]:
                    if rid in job["errors"]:
                        continue
                    path = report(root, rid)
                    content = read(path)
                    assert content["source"]["dialogue_id"] == rid
                    assert content["source"]["user_id"] == "user-a"
                    digest = hashlib.sha256(path.read_bytes()).hexdigest()
                    assert state(root, rid)["learning"]["report_fingerprint"] == digest
                    material += rid + ":" + digest + "\n"
                    saved[rid] = (path.read_bytes(), path.stat().st_mtime_ns)
                assert job["reports_fingerprint"] == hashlib.sha256(material.encode()).hexdigest()
            # 重启不重复请求成功轮，也不擅自重试未获预算的失败轮。
            with service(root):
                time.sleep(0.15)
                assert Model.calls == before + 6
            assert all((report(root, rid).read_bytes(), report(root, rid).stat().st_mtime_ns)
                       == value for rid, value in saved.items())
        finally:
            start.set()
            slow.set()
            Model.response_factory = None


def parallel_analysis_budget():
    """并发设为 32 也不能突破每批一次的模型预算，剩余轮进入后续批次。"""
    root = retry_fixture("parallel-budget", min_new_runs=3, max_wait_ms=300,
                         analysis_parallelism=32)
    path = root / "config/standard-runtime.json"
    runtime = read(path)
    runtime["evaluation"]["max_model_calls"] = 1
    write(path, runtime)
    Model.response_factory = analysis_reply
    before = Model.calls
    try:
        with service(root) as (call, _):
            ids = [run(call) for _ in range(3)]
            wait(lambda: done(root, ids))
            assert Model.calls == before + 3
            assert len(jobs(root)) == 3
            assert all(len(job["runs"]) == 1 and not job["errors"] for job in jobs(root))
    finally:
        Model.response_factory = None


def parallel_analysis_native():
    """通过原生入口实测 16 个分析请求同时在途，主入口仍可查询契约。"""
    root = retry_fixture("parallel-native", min_new_runs=16, max_batch_runs=16,
                         analysis_parallelism=16)
    config = read(root / "service.json")
    config["max_active_runs"] = 32
    write(root / "service.json", config)
    spec = importlib.util.spec_from_file_location("parallel_local", SOURCE / "clients/python/evolution_local.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    gate = threading.Event()
    lock = threading.Lock()
    arrived = []
    before = Model.calls
    client = None

    def reply(raw):
        with lock:
            arrived.append(True)
        assert gate.wait(15), "native analysis did not reach configured concurrency"
        return analysis_reply(raw)

    Model.response_factory = reply
    try:
        client = module.LocalEvolution(BUILD / "libtrajectory_evolution_local.so",
                                       root / "service.json", TOKENS["user-a"],
                                       contention_budget_us=10000)

        def call(operation, user="user-a", **fields):
            assert user == "user-a"
            return client.invoke(operation, fields)

        ids = [run(call) for _ in range(16)]
        wait(lambda: len(arrived) == 16, timeout=20)
        # 原生热槽位可以复用，旧 run 的持久化以文件为准，不假设仍能查热槽位。
        assert all((root / "data/runs" / (rid + ".json")).exists() for rid in ids)
        assert client.invoke("contract.describe", {})["transport"] == "local.native.v1"
        gate.set()
        wait(lambda: done(root, ids))
        assert Model.calls == before + 16
        assert len(jobs(root)) == 1 and not jobs(root)[0]["errors"]
    finally:
        gate.set()
        if client is not None:
            client.close()
        Model.response_factory = None


def parallel_analysis_configuration():
    """拒绝零、超限、负数和非整数并发，避免无工作者或不受控创建线程。"""
    for index, value in enumerate((0, 33, -1, 1.5, "4")):
        root = fixture("parallel-invalid-" + str(index), analysis_parallelism=value)
        result = subprocess.run([str(BUILD / "trajectory_evolution"), "serve", "--config",
                                 str(root / "service.json")], env=ENV, capture_output=True,
                                text=True, timeout=10)
        assert result.returncode != 0, value


MODEL = ThreadingHTTPServer(("127.0.0.1", 0), Model)
Model.release.set()
thread = threading.Thread(target=MODEL.serve_forever, daemon=True)
thread.start()
try:
    for test in (
        batching,
        deadlines,
        configuration_backoff,
        busy_arrivals,
        interrupted_analysis,
        strict_error_codes,
        checkpoints,
        isolated_failure,
        publication_failure,
        bounded_batches,
        idle_cache,
        archived_jobs_restart,
        repeated_shutdown_drains,
        automatic_retry_restart_and_cli,
        automatic_retry_limits,
        automatic_retry_local_runtime,
        automatic_task_outcome_retry,
        automatic_episode_count_retry,
        automatic_dialogue_protocol_retry,
        protocol_retry_boundaries,
        parallel_analysis,
        parallel_analysis_budget,
        parallel_analysis_native,
        parallel_analysis_configuration,
    ):
        test()
        print(test.__name__ + ": passed", flush=True)
    for path in (ROOT).rglob("*"):
        if path.is_file() and "data" in path.parts:
            assert all(
                token.encode() not in path.read_bytes() for token in TOKENS.values()
            ), path
    result = {
        "checks": 24,
        "paid_provider_calls": 0,
        "fixture_model_calls": Model.calls,
        "output": str(ROOT),
    }
    write(ROOT / "test-summary.json", result)
    print(json.dumps(result))
finally:
    Model.release.set()
    MODEL.shutdown()
    MODEL.server_close()
    thread.join(timeout=3)
