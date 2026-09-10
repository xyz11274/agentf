"""Durable incremental learning, cadence and restart tests with a local fake model."""
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
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
TOKENS = {"user-a": "fixture-principal-alpha-00000001", "user-b": "fixture-principal-bravo-00000002"}
ENV = dict(os.environ, INTEGRATION_TEST_A=TOKENS["user-a"], INTEGRATION_TEST_B=TOKENS["user-b"])
os.environ.update(INTEGRATION_TEST_A=TOKENS["user-a"], INTEGRATION_TEST_B=TOKENS["user-b"])


def read(path):
    return json.loads(path.read_text())


def write(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False))


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

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", 0)))
        type(self).calls += 1
        type(self).entered.set()
        type(self).release.wait(20)
        try:
            self.send_response(503)
            self.end_headers()
            self.wfile.write(b"fixture model unavailable")
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *args):
        pass


def fixture(name, **learning):
    root = ROOT / name
    subprocess.run([str(BUILD / "test_integration_service"), "--fixture", str(root)],
                   env=ENV, check=True, capture_output=True, timeout=20)
    shutil.copytree(SOURCE / "config", root / "config")
    config = read(root / "service.json")
    config.update(port=0, max_active_runs=2, runtime_config=str(root / "config/standard-runtime.json"))
    config["learning"].update(min_new_runs=1, max_wait_ms=60000, min_interval_ms=0, retry_backoff_ms=10)
    config["learning"].update(learning)
    write(root / "service.json", config)
    runtime = read(root / "config/standard-runtime.json")
    runtime["evaluation"].update(model_selection="all")
    runtime["model"].update(endpoint=f"http://127.0.0.1:{MODEL.server_port}/v1/chat/completions",
                            model_id="fixture-model", no_api_key=True, max_retries=0, timeout_ms=25000)
    write(root / "config/standard-runtime.json", runtime)
    return root


@contextmanager
def service(root):
    with (root / "server.stderr").open("a") as errors:
        process = subprocess.Popen([str(BUILD / "trajectory_evolution"), "serve", "--config", str(root / "service.json")],
                                   env=ENV, stdout=subprocess.PIPE, stderr=errors, text=True)
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                assert selector.select(10), "startup timeout"
            line = process.stdout.readline()
            match = re.search(r"listening=127\.0\.0\.1:(\d+)", line)
            assert match, (line, (root / "server.stderr").read_text())
            endpoint = "http://127.0.0.1:" + match.group(1) + "/v1/invoke"

            def call(operation, user="user-a", **fields):
                body = dict(fields, operation=operation, request_id=uuid.uuid4().hex)
                request = urllib.request.Request(endpoint, json.dumps(body).encode(),
                    {"Content-Type": "application/json", "Authorization": "Bearer " + TOKENS[user]})
                with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=5) as reply:
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


def run(call, user="user-a", close=True):
    rid = call("run.begin", user=user, input="检查独立分支与失败回执", level="L0", conversation_id="conversation")["run_id"]
    activity = call("activity.begin", user=user, run_id=rid, kind="model", name="reason", input="保持原始请求")
    call("activity.end", user=user, run_id=rid, activity_id=activity["activity_id"], status="completed",
         executed=True, actual_input=activity["input"], output="尝试查询")
    if close:
        call("run.end", user=user, run_id=rid, status="completed", business_status="partial", output="已完成")
    return rid


def state(root, rid):
    return read(root / "data/runs" / (rid + ".json"))


def done(root, ids):
    return all(state(root, rid).get("evaluation_status") == "completed" for rid in ids)


def jobs(root):
    return [read(path) for path in (root / "data/learning/jobs").glob("*.json")]


def report(root, rid):
    return next((root / "data/by-user").glob("*/evaluation/*/" + rid + ".evaluation.json"))


def batching():
    before = Model.calls
    root = fixture("count-and-restart", min_new_runs=3)
    with service(root) as (call, _):
        ids = [run(call), run(call)]
        unfinished = run(call, close=False)
        time.sleep(0.15)
        assert Model.calls == before and not jobs(root), "run.begin or a small batch triggered learning"
        call("run.end", run_id=unfinished, status="completed", output="终态")
        ids.append(unfinished)
        wait(lambda: done(root, ids))
        assert Model.calls == before + 3 and len(jobs(root)) == 1
        saved = {rid: (report(root, rid).read_bytes(), report(root, rid).stat().st_mtime_ns) for rid in ids}
        # More completed-but-pending runs than max_active_runs must not block producers.
        later = [run(call, "user-b"), run(call, "user-b")]
    with service(root) as (call, _):
        time.sleep(0.15)
        assert Model.calls == before + 3, "restart replayed old reports or flushed the small batch"
        later.append(run(call, "user-b"))
        wait(lambda: done(root, later))
        assert Model.calls == before + 6
    assert all((report(root, rid).read_bytes(), report(root, rid).stat().st_mtime_ns) == saved[rid] for rid in ids)
    assert {job["user"] for job in jobs(root)} == {"user-a", "user-b"}
    assert all(len(job["runs"]) == 3 for job in jobs(root))


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
        assert state(root, second)["evaluation_status"] == "pending", "restart lost cooldown"
        time.sleep(0.15)
        assert Model.calls == before
        wait(lambda: done(root, [second]))


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
        wait(lambda: schedule.exists() and read(schedule).get("last_error") == "evaluation_configuration_failed")
        assert state(root, rid)["evaluation_status"] == "pending" and not jobs(root)
        saved_time = schedule.stat().st_mtime_ns
    write(path, good)
    with service(root):
        time.sleep(0.15)
        assert Model.calls == before and schedule.stat().st_mtime_ns == saved_time, "restart discarded storage/configuration backoff"
        wait(lambda: done(root, [rid]))
        assert Model.calls == before + 1


def busy_arrivals():
    root = fixture("busy", min_new_runs=2)
    Model.entered.clear(); Model.release.clear()
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


def interrupted_analysis():
    root = fixture("interrupted-analysis")
    Model.entered.clear(); Model.release.clear()
    before = Model.calls
    try:
        with service(root) as (call, process):
            rid = run(call)
            assert Model.entered.wait(5)
            process.kill(); process.wait(5)
    finally:
        Model.release.set()
    with service(root) as (call, _):
        wait(lambda: state(root, rid)["evaluation_status"] == "failed")
        assert state(root, rid)["evaluation_error"] == "analysis_interrupted_unknown_outcome"
        assert Model.calls == before + 1, "ambiguous crashed request was automatically rebilled"
        call("evaluation.retry", run_id=rid)
        wait(lambda: done(root, [rid]))
        assert Model.calls == before + 2


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
    revision = read(root / "data/by-user/user-a/memory/evolution-memory.json")["revision"]
    # Simulate process death after memory commit, before per-run completion.
    job.update(state="running", phase="memory_curation")
    write(root / "data/learning/jobs" / (job["id"] + ".json"), job)
    value = state(root, rid); value["evaluation_status"] = "running"
    write(root / "data/runs" / (rid + ".json"), value)
    with service(root):
        wait(lambda: done(root, [rid]))
        assert Model.calls == before + 1 and candidate.stat().st_mtime_ns == candidate_time
        assert read(root / "data/by-user/user-a/memory/evolution-memory.json")["revision"] == revision
    # A completed report deleted by an operator must not cause silent reanalysis.
    report(root, rid).unlink()
    job.update(state="running", phase="memory_curation")
    write(root / "data/learning/jobs" / (job["id"] + ".json"), job)
    value = state(root, rid); value["evaluation_status"] = "running"
    write(root / "data/runs" / (rid + ".json"), value)
    with service(root):
        wait(lambda: state(root, rid)["evaluation_status"] == "failed")
        assert state(root, rid)["evaluation_error"] == "analysis_cache_missing"
        assert Model.calls == before + 1


def isolated_failure():
    root = fixture("isolated-failure", min_new_runs=2)
    before = Model.calls
    with service(root) as (call, _):
        first = run(call)
        wait(lambda: state(root, first)["learning"].get("trace_published"))
        path = root / "data/by-user/user-a/evaluation/conversation" / (first + ".evaluation.json")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("not valid JSON")
        second = run(call)
        wait(lambda: state(root, first)["evaluation_status"] == "failed" and done(root, [second]))
        assert Model.calls == before + 1, "bad cache must not be silently overwritten"
        assert len(jobs(root)[0]["errors"]) == 1


def publication_failure():
    root = fixture("publication-failure")
    blocked = root / "data/by-user/user-a/traces"
    blocked.write_text("fixture obstruction")
    with service(root) as (call, _):
        first = run(call)
        wait(lambda: state(root, first)["evaluation_status"] == "failed")
        assert state(root, first)["learning"]["publication_error"] == "trace_publication_failed"
        second = run(call, "user-b")
        wait(lambda: done(root, [second]))
        blocked.rename(blocked.with_name("traces.obstruction"))
        call("evaluation.retry", run_id=first)
        wait(lambda: done(root, [first]))


def bounded_batches():
    root = fixture("model-budget", min_new_runs=3, max_wait_ms=300, max_batch_runs=3)
    path = root / "config/standard-runtime.json"
    config = read(path); config["evaluation"]["max_model_calls"] = 2
    write(path, config)
    before = Model.calls
    with service(root) as (call, _):
        ids = [run(call) for _ in range(3)]
        wait(lambda: done(root, ids))
    assert Model.calls == before + 3 and sorted(len(job["runs"]) for job in jobs(root)) == [1, 2]
    assert all(sum(job["analysis_model_calls"].values()) <= 2 for job in jobs(root))


def idle_cache():
    root = fixture("idle-cache", min_new_runs=3)
    spec = importlib.util.spec_from_file_location("local", SOURCE / "clients/python/evolution_local.py")
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    client = module.LocalEvolution(BUILD / "libtrajectory_evolution_local.so", root / "service.json", TOKENS["user-a"])
    try:
        initial = client.metrics()["memory_refreshes"]
        time.sleep(0.7)
        assert client.metrics()["memory_refreshes"] == initial == 1, "idle cache was reread"
        rid = client.invoke("run.begin", {"input": "只收集，不触发学习", "level": "L0"})["run_id"]
        client.invoke("run.end", {"run_id": rid, "status": "completed", "output": "完成"})
        wait(lambda: client.invoke("run.status", {"run_id": rid})["persisted"])
        time.sleep(0.15)
        assert client.metrics()["memory_refreshes"] == initial and not jobs(root)
        for _ in range(2):
            rid = client.invoke("run.begin", {"input": "新的完整样本", "level": "L0"})["run_id"]
            client.invoke("run.end", {"run_id": rid, "status": "completed", "output": "完成"})
            wait(lambda: client.invoke("run.status", {"run_id": rid})["persisted"])
        wait(lambda: client.metrics()["memory_refreshes"] == initial + 1)
        time.sleep(0.2)
        assert client.metrics()["memory_refreshes"] == initial + 1
    finally:
        client.close()


MODEL = ThreadingHTTPServer(("127.0.0.1", 0), Model)
Model.release.set()
thread = threading.Thread(target=MODEL.serve_forever, daemon=True); thread.start()
try:
    for test in (batching, deadlines, configuration_backoff, busy_arrivals, interrupted_analysis, checkpoints, isolated_failure,
                 publication_failure, bounded_batches, idle_cache):
        test()
        print(test.__name__ + ": passed", flush=True)
    for path in (ROOT).rglob("*"):
        if path.is_file() and "data" in path.parts:
            assert all(token.encode() not in path.read_bytes() for token in TOKENS.values()), path
    result = {"checks": 10, "paid_provider_calls": 0, "fixture_model_calls": Model.calls, "output": str(ROOT)}
    write(ROOT / "test-summary.json", result)
    print(json.dumps(result))
finally:
    Model.release.set(); MODEL.shutdown(); MODEL.server_close(); thread.join(timeout=3)
