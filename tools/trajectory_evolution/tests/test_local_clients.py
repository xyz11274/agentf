"""Native clients: background faults, real receipts, privacy, and process recovery."""
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

SOURCE = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv[1]).resolve()
ROOT = Path(tempfile.mkdtemp(prefix="local-clients.", dir=BUILD))
TOKEN_A = "fixture-principal-alpha-00000001"
TOKEN_B = "fixture-principal-bravo-00000002"
os.environ.update(INTEGRATION_TEST_A=TOKEN_A, INTEGRATION_TEST_B=TOKEN_B)
spec = importlib.util.spec_from_file_location("evolution_local", SOURCE / "clients/python/evolution_local.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
LIBRARY = BUILD / "libtrajectory_evolution_local.so"


def fixture(name):
    root = ROOT / name
    subprocess.run([str(BUILD / "test_integration_service"), "--fixture", str(root)],
                   check=True, capture_output=True, timeout=20)
    shutil.copytree(SOURCE / "config", root / "config")
    config = json.loads((root / "service.json").read_text())
    config["runtime_config"] = str(root / "config/standard-runtime.json")
    config["runtime_options"]["loop_rules"] = []
    (root / "service.json").write_text(json.dumps(config))
    return root


def client(root, token=TOKEN_A, **options):
    return module.LocalEvolution(LIBRARY, root / "service.json", token, **options)


def call(c, op, **fields):
    result = c.invoke(op, fields)
    assert not result.get("degraded"), (op, result)
    return result


def begin(c, conv="native", level="L3"):
    return call(c, "run.begin", conversation_id=conv, level=level,
                capabilities=["model", "actions", "loop"], input="视频生成缺少素材")["run_id"]


def complete(c, run, rounds=3, tamper=False):
    for i in range(rounds):
        control = call(c, "loop.check", run_id=run, loop_id="planner", state={"round": i})
        call(c, "loop.commit", run_id=run, control_id=control["control_id"], decision=control["decision"])
        model = call(c, "activity.begin", run_id=run, kind="model", name="reason", input={"system": "原始指令"},
                     binding={"pointer": "/system", "mode": "text"}, control_id=control["control_id"])
        actual = {"system": "宿主错误地丢弃经验"} if tamper else model["input"]
        call(c, "activity.end", run_id=run, activity_id=model["activity_id"], actual_input=actual,
             executed=True, status="completed", output="并行检索后继续")
        a = call(c, "activity.begin", run_id=run, kind="action", name="lookup", input={"branch": "a"})
        b = call(c, "activity.begin", run_id=run, kind="action", name="lookup", input={"branch": "b"})
        call(c, "activity.end", run_id=run, activity_id=b["activity_id"], executed=True, status="failed", output="业务分支失败")
        call(c, "activity.end", run_id=run, activity_id=a["activity_id"], executed=True, status="completed", output="独立分支成功")
    end = call(c, "run.end", run_id=run, status="completed", business_status="partial", output="部分完成")
    assert end["persisted"] is False


def wait_for(predicate, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.03)
    raise AssertionError("background completion timeout")


def valid_traces(root, count):
    paths = list((root / "data/by-user").glob("*/traces/**/*.trace.json"))
    assert len(paths) == count, (root, len(paths), count)
    for path in paths:
        subprocess.run([str(BUILD / "trajectory_evolution"), "doctor", str(path), "--level", "L0"],
                       check=True, capture_output=True, timeout=20)


def multi_user():
    root = fixture("multi-user")
    a, b = client(root), client(root, TOKEN_B)
    try:
        def work(c, conversations):
            for conv in conversations:
                for turn in range(3):
                    run = begin(c, conv)
                    complete(c, run, tamper=(turn == 2 and c is a))
        with ThreadPoolExecutor(max_workers=2) as pool:
            jobs = [pool.submit(work, a, ["alpha-1", "alpha-2"]), pool.submit(work, b, ["bravo-1"])]
            for job in jobs:
                job.result(timeout=30)
        wait_for(lambda: len(list((root / "data/by-user").glob("*/evaluation/**/*.evaluation.json"))) == 9, 30)
        valid_traces(root, 9)
        runs = [json.loads(p.read_text()) for p in (root / "data/runs").glob("*.json")]
        assert any(r["receipt_counts"].get("failed", 0) for r in runs), "actual input mismatch was not detected"
        assert any(r["receipt_counts"].get("applied", 0) for r in runs), "real policy application missing"
        assert all(r["local_capture"] for r in runs)
        assert a.metrics()["degraded_calls"] == b.metrics()["degraded_calls"] == 0
    finally:
        a.close(); b.close()


def capacity_and_concurrency():
    root = fixture("capacity")
    c = client(root, background=False, max_pending_runs=1)
    try:
        contract = c.invoke("contract.describe", {})
        assert contract["transport"] == "local.native.v1" and "evaluation.retry" not in contract["operations"]
        assert contract["limits"]["max_request_bytes"] == 65536
        run = begin(c)
        started = time.monotonic()
        rejected = c.invoke("run.begin", {"conversation_id": "full", "input": "overflow"})
        assert rejected["reason"] == "local_capacity_reached" and time.monotonic() - started < 0.2
        # True simultaneous producers must be truthful on contention, not silently lose events.
        barrier = threading.Barrier(4)
        def record(i):
            barrier.wait(timeout=5)
            return c.invoke("event.record", {"run_id": run, "type": "probe", "data": {"index": i}})
        with ThreadPoolExecutor(max_workers=4) as pool:
            responses = list(pool.map(record, range(4)))
        end = c.invoke("run.end", {"run_id": run, "status": "completed", "output": "finished"})
        c.drain()
        state = json.loads((root / "data/runs" / (run + ".json")).read_text())
        if any(r.get("degraded") for r in responses):
            assert end["degraded"] and state["incomplete"] and state["evaluation_status"] == "quarantined"
        else:
            assert not end["degraded"]
        return sum(bool(r.get("degraded")) for r in responses)
    finally:
        c.close()


def disk_fault():
    root = fixture("disk-fault")
    c = client(root)
    runs = root / "data/runs"
    saved = root / "data/runs.saved"
    try:
        runs.rename(saved)
        runs.write_text("fixture preventing directory writes")
        start = time.monotonic()
        run = begin(c)
        complete(c, run, rounds=1)
        elapsed = time.monotonic() - start
        wait_for(lambda: c.metrics()["persistence_errors"] > 0)
        assert elapsed < 0.5 and c.metrics()["degraded_calls"] == 0
        runs.unlink(); saved.rename(runs)
        wait_for(lambda: c.metrics()["published_runs"] == 1)
        wait_for(lambda: len(list((root / "data/by-user").glob("*/evaluation/**/*.evaluation.json"))) == 1)
        valid_traces(root, 1)
        return elapsed
    finally:
        if saved.exists():
            if runs.is_file():
                runs.unlink()
            saved.rename(runs)
        c.close()


def slow_model():
    entered, release = threading.Event(), threading.Event()
    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):
            self.rfile.read(int(self.headers.get("Content-Length", 0)))
            entered.set(); release.wait(15)
            self.send_response(503); self.end_headers(); self.wfile.write(b"fixture model unavailable")
        def log_message(self, *args):
            pass
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True); worker.start()
    root = fixture("slow-model")
    path = root / "config/standard-runtime.json"
    config = json.loads(path.read_text())
    config["evaluation"]["model_selection"] = "all"
    config["model"].update(endpoint=f"http://127.0.0.1:{server.server_port}/v1/chat/completions",
                           model_id="fixture-model", no_api_key=True, max_retries=0, timeout_ms=20000)
    path.write_text(json.dumps(config))
    c = client(root)
    try:
        complete(c, begin(c), rounds=1)
        assert entered.wait(10), "background evaluator did not call fixture model"
        start = time.monotonic()
        complete(c, begin(c, "while-model-blocked"), rounds=2)
        elapsed = time.monotonic() - start
        assert not release.is_set() and elapsed < 0.5 and c.metrics()["degraded_calls"] == 0
        return elapsed
    finally:
        release.set(); c.close(); server.shutdown(); server.server_close(); worker.join(timeout=3)


def privacy_and_restart():
    root = fixture("privacy")
    c = client(root, background=False)
    try:
        run = begin(c)
        rejected = c.invoke("event.record", {"run_id": run, "type": "credential", "data": {"api_key": "fixture-private-payload"}})
        assert rejected["degraded"] and not rejected["policy_applied"]
        c.invoke("run.end", {"run_id": run, "status": "completed", "output": "done"}); c.drain()
        oversized = c.invoke("run.begin", {"input": "x" * 1000000})
        assert oversized["degraded"]
        c.set_enabled(False)
        assert c.invoke("run.begin", {"input": "paused"})["degraded"]
        c.set_enabled(True)
        assert c.invoke("activity.begin", {"run_id": run, "input": float("nan")})["degraded"]
    finally:
        c.close()
    for p in (root / "data").rglob("*"):
        if p.is_file():
            assert b"fixture-private-payload" not in p.read_bytes()
    root = fixture("interrupted")
    script = SOURCE / "tests/clients/local_crash.py"
    subprocess.run([sys.executable, "-B", str(script), str(LIBRARY), str(root / "service.json")],
                   check=True, capture_output=True, timeout=15)
    c = client(root)
    try:
        wait_for(lambda: bool(list((root / "data/by-user").glob("*/quarantine/**/*.trace.json"))))
        state = json.loads(next((root / "data/runs").glob("*.json")).read_text())
        assert state["evaluation_status"] == "quarantined" and state["incomplete"]
    finally:
        c.close()


def authorization_and_reuse():
    root = fixture("authorization")
    path = root / "service.json"
    config = json.loads(path.read_text()); config["auto_approve_experiences"] = False
    path.write_text(json.dumps(config))
    c = client(root, background=False, max_pending_runs=1)
    try:
        run = begin(c, level="L1")
        rejected = c.invoke("run.begin", {"input": {"api_key": "fixture-private-new-run"}})
        assert rejected["degraded"], "sensitive new run was accepted"
        # A rejected new task must not taint a different, already active task.
        model = call(c, "activity.begin", run_id=run, kind="model", name="reason", input={"system": "原始指令"},
                     binding={"pointer": "/system", "mode": "text"})
        assert model["input"] == {"system": "原始指令"}, "unapproved memory was injected"
        call(c, "activity.end", run_id=run, activity_id=model["activity_id"], actual_input=model["input"],
             executed=True, status="completed", output="完成")
        call(c, "run.end", run_id=run, status="completed", output="完成"); c.drain()
        ids = []
        for request_id in ["old-start", "new-start", "old-start"]:
            packet = {"input": "L0 reuse fixture", "level": "L0"}
            result = c.invoke("run.begin", packet, request_id)
            assert not result["degraded"]
            assert c.invoke("run.begin", packet, request_id) == result, "retained idempotency failed"
            ids.append(result["run_id"])
            call(c, "run.end", run_id=result["run_id"], status="completed", output="完成"); c.drain()
        assert len(set(ids)) == 3, "expired cache overwrote a previous durable run"
    finally:
        c.close()


def java_native_contract():
    maven = shutil.which("mvn")
    if not maven:
        return False
    root = fixture("java-native")
    command = [maven, "-q", "-f", str(SOURCE / "clients/java/pom.xml"),
               "-Dtest=LocalEvolutionNativeContractTest", f"-Devolution.native.library={LIBRARY}",
               f"-Devolution.native.config={root / 'service.json'}", "test"]
    cache = (BUILD / "CMakeCache.txt").read_text()
    compiler = re.search(r"^CMAKE_CXX_COMPILER:[^=]+=(.+)$", cache, re.MULTILINE).group(1)
    runtime = subprocess.run([compiler, "-print-file-name=libstdc++.so.6"],
                             check=True, capture_output=True, text=True).stdout.strip()
    env = os.environ.copy()
    if Path(runtime).is_file():
        env["LD_LIBRARY_PATH"] = str(Path(runtime).resolve().parent) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    result = subprocess.run(command, capture_output=True, text=True, timeout=60, env=env)
    assert result.returncode == 0, result.stdout + result.stderr
    valid_traces(root, 1)
    return True


def main():
    multi_user()
    busy = capacity_and_concurrency()
    disk = disk_fault()
    model = slow_model()
    privacy_and_restart()
    authorization_and_reuse()
    node = shutil.which("node")
    tested = ["python"]
    if java_native_contract():
        tested.append("java")
    if node and (BUILD / "trajectory_evolution_local_node.node").exists():
        root = fixture("javascript")
        # Node loads its C++ runtime before dlopen; match the library's compiler.
        cache = (BUILD / "CMakeCache.txt").read_text()
        compiler = re.search(r"^CMAKE_CXX_COMPILER:[^=]+=(.+)$", cache, re.MULTILINE).group(1)
        runtime = subprocess.run([compiler, "-print-file-name=libstdc++.so.6"],
                                 check=True, capture_output=True, text=True).stdout.strip()
        env = dict(os.environ)
        if Path(runtime).is_file():
            env["LD_LIBRARY_PATH"] = str(Path(runtime).resolve().parent) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
        result = subprocess.run([node, str(SOURCE / "tests/clients/local.mjs"), str(BUILD), str(root / "service.json")],
                                capture_output=True, text=True, timeout=20, env=env)
        assert result.returncode == 0, result.stderr
        valid_traces(root, 1); tested.append("javascript")
    for p in ROOT.rglob("*"):
        if p.is_file():
            data = p.read_bytes()
            assert TOKEN_A.encode() not in data and TOKEN_B.encode() not in data, "principal credential persisted"
    summary = {"output": str(ROOT), "languages": tested, "users": 2, "conversations": 3, "runs": 9,
               "same_run_contention_degraded_calls": busy, "blocked_disk_business_seconds": disk,
               "blocked_model_business_seconds": model, "privacy_checked": True,
               "interrupted_run_quarantined": True, "paid_provider_calls": 0}
    (ROOT / "test-summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
