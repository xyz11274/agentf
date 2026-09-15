"""Real file/queue/restart integration; model responses are deterministic fixtures."""
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import copy
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
import urllib.request
import uuid

SOURCE = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv[1]).resolve()
ROOT = Path(tempfile.mkdtemp(prefix="unified-memory.", dir=BUILD))
ENV = dict(os.environ, INTEGRATION_TEST_A="fixture-principal-alpha-00000001",
           INTEGRATION_TEST_B="fixture-principal-bravo-00000002")
SCOPE = "agent:my-agent/actor:assistant"
CONDITION = "视频生成缺少图片素材"
ADVICE = "缺少图片素材时先请求补充，独立检索继续。"


def read(path):
    return json.loads(path.read_text())


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False))


def command(*args, expected=0, data=None):
    result = subprocess.run([str(BUILD / "trajectory_evolution"), *map(str, args)], env=ENV,
                            input=data, capture_output=True, timeout=35)
    assert result.returncode == expected, (args, result.returncode, result.stdout.decode(), result.stderr.decode(), ROOT)
    return result


def fixture(name):
    root = ROOT / name
    subprocess.run([str(BUILD / "test_integration_service"), "--fixture", str(root)],
                   env=ENV, capture_output=True, check=True, timeout=20)
    shutil.copytree(SOURCE / "config", root / "config")
    config = read(root / "service.json")
    config.update(port=0, runtime_config=str(root / "config/standard-runtime.json"),
                  sharing={"enabled": True, "min_interval_ms": 10, "min_supporting_users": 2})
    write(root / "service.json", config)
    return root


def trace(user, run):
    result = read(SOURCE / "examples/standard-agent/one.trace.json")
    result["identity"] = dict(agent_id="my-agent", user_id=user, run_id=run, conversation_id="conversation-" + user)
    return result


def learn(root, path=None, expected=0, *flags):
    args = ["learn"] + ([path] if path else []) + ["--config", root / "service.json", *flags]
    result = command(*args, expected=expected)
    return json.loads(result.stdout)


class Model(BaseHTTPRequestHandler):
    calls = 0
    requests = []

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        payload = json.loads(body["messages"][-1]["content"])
        type(self).calls += 1
        type(self).requests.append(payload)
        if "draft" in payload:
            existing = payload["existing"]
            response = dict(decision="duplicate" if existing else "new", match_id=existing[0]["memory_id"] if existing else "",
                            privacy_safe=True, generalizable=True, supported=True, non_overriding=True, confidence=0.99)
        elif "source" in payload:
            response = dict(shareable=True, target_scope=SCOPE, trigger_pattern=CONDITION, content=ADVICE)
        else:
            assert "private" in payload and "public" in payload, payload
            response = dict(relation="duplicate", confidence=0.99)
        encoded = json.dumps({"choices": [{"message": {"role": "assistant", "content": json.dumps(response, ensure_ascii=False)},
                                           "finish_reason": "stop"}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def log_message(self, *args):
        pass


def model_config(root, port):
    config = read(root / "config/standard-runtime.json")
    config["evaluation"]["model_selection"] = "none"
    config["evolution"].update(enable_meta_reflection=False, enable_memory_semantic_matching=False)
    config["model"].update(endpoint=f"http://127.0.0.1:{port}/v1/chat/completions", model_id="test-public-curator",
                           no_api_key=True, max_retries=0, timeout_ms=2000)
    write(root / "config/standard-runtime.json", config)


def seed_verified_memory(root, user):
    # Deliberately a fixture, not a fabricated result of model reflection.
    # Every reference still points to an actually imported/evaluated immutable trace.
    reports = [read(p) for p in (root / "data/by-user" / user / "evaluation").rglob("*.evaluation.json")]
    assert len(reports) >= 2
    encoded = subprocess.run([str(BUILD / "test_shared_memory"), "--seed-reports"],
                             input=json.dumps(dict(scope=SCOPE, condition=CONDITION, reports=reports)).encode(),
                             capture_output=True, check=True).stdout
    (root / "data/by-user" / user / "memory/evolution-memory.pb").write_bytes(encoded)


@contextmanager
def service(root):
    with (root / "server.stderr").open("a") as errors:
        process = subprocess.Popen([str(BUILD / "trajectory_evolution"), "serve", "--config", str(root / "service.json")],
                                   env=ENV, stdout=subprocess.PIPE, stderr=errors, text=True)
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                assert selector.select(10), "startup timed out"
            match = re.search(r"listening=127\.0\.0\.1:(\d+)", process.stdout.readline())
            assert match, (root / "server.stderr").read_text()
            endpoint = "http://127.0.0.1:" + match.group(1) + "/v1/invoke"

            def call(operation, user="user-a", **fields):
                request = urllib.request.Request(endpoint, json.dumps(dict(fields, operation=operation, request_id=uuid.uuid4().hex)).encode(),
                    {"Content-Type": "application/json", "Authorization": "Bearer " + ENV["INTEGRATION_TEST_A" if user == "user-a" else "INTEGRATION_TEST_B"]})
                with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=5) as reply:
                    return json.load(reply)["result"]
            yield call
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(5)


def import_and_share(port):
    root = fixture("two-users")
    logs = root / "input"
    for user in ("user-a", "user-b"):
        for n in range(2):
            write(logs / f"{user}-{n}.trace.json", trace(user, f"task-{user}-{n}"))
    queued = learn(root, logs, 0, "--enqueue-only")
    assert queued["imported"] == 4 and queued["pending_runs"] == 4, queued
    done = learn(root)
    assert done["pending_runs"] == 0 and done["run_outcomes"].get("completed") == 4, done
    assert done["managed_users"] == 2
    assert (root / "data/memory/evolution-memory.pb").is_file()
    evaluations = {p: hashlib.sha256(p.read_bytes()).hexdigest()
                   for p in (root / "data/by-user").glob("*/evaluation/*/*.evaluation.json")}
    assert len(evaluations) == 4
    again = learn(root, logs)
    assert again["reused"] == 4 and again["imported"] == 0
    assert all(hashlib.sha256(p.read_bytes()).hexdigest() == h for p, h in evaluations.items())

    one = read(logs / "user-a-0.trace.json")
    encoded = subprocess.run([str(BUILD / "test_shared_memory"), "--encode-trace"],
                             input=json.dumps(one).encode(), capture_output=True, check=True).stdout
    binary = root / "same.trace.pb"
    binary.write_bytes(encoded)
    assert learn(root, binary)["reused"] == 1, "JSON/PB duplicate was re-evaluated"

    for user in ("user-a", "user-b"):
        seed_verified_memory(root, user)
    model_config(root, port)
    before = Model.calls
    learned = learn(root)
    public = read(root / "data/memory/evolution-memory.json")
    assert len(public.get("experiences", [])) == 1, (learned, read(root / "data/learning/shared-memory.json"))
    assert 4 <= Model.calls - before <= 8, Model.calls - before
    for forbidden in ("user-a", "user-b", "private-", "candidate-", "私人历史", "task-user"):
        assert forbidden not in json.dumps(public, ensure_ascii=False), forbidden
        assert forbidden not in json.dumps(Model.requests, ensure_ascii=False), forbidden
    before = Model.calls
    learn(root)
    assert Model.calls == before, "restart repeated unchanged model review"

    with service(root) as call:
        a = call("memory.status")
        b = call("memory.status", user="user-b")
        assert a["user_id"] == "user-a" and b["user_id"] == "user-b"
        assert a["public_entries"].get("published") == 1
        # The online path reads the exact same memory as offline ingestion.
        begin = call("run.begin", input=CONDITION, conversation_id="online", level="L1", capabilities=["model"])
        activity = call("activity.begin", run_id=begin["run_id"], kind="model", name="reason", actor_id="assistant",
                        input={"system": "你是助手", "query": CONDITION}, binding={"pointer": "/system", "mode": "text"})
        assert ADVICE in activity["input"]["system"], activity
        assert activity["input"]["system"].count(ADVICE) == 1, "duplicate cross-pool injection"
        call("activity.end", run_id=begin["run_id"], activity_id=activity["activity_id"], status="completed",
             actual_input=activity["input"], executed=True, output="请补充图片")
        call("run.end", run_id=begin["run_id"], status="completed", output="请补充图片")
    return root, logs


def rejection_and_recovery(root, logs):
    before = len(list((root / "data/runs").glob("*.json")))
    cases = []
    unknown = trace("unconfigured", "unknown"); cases.append(unknown)
    foreign = trace("user-a", "foreign"); foreign["identity"]["agent_id"] = "other-app"; cases.append(foreign)
    changed = read(logs / "user-a-0.trace.json"); changed["events"][0]["input"] = "changed immutable task"; cases.append(changed)
    incomplete = trace("user-a", "incomplete"); incomplete["events"].pop(); cases.append(incomplete)
    secret = trace("user-a", "sensitive"); secret["events"][0]["input"] = {"nested": [{"api_key": "unknown-format-fixture-secret"}]}; cases.append(secret)
    for n, value in enumerate(cases):
        path = root / f"invalid-{n}.trace.json"; write(path, value)
        result = learn(root, path, 3, "--enqueue-only")
        assert result["rejected"] == 1 and result["imported"] == 0, result
    assert len(list((root / "data/runs").glob("*.json"))) == before
    for path in (root / "data").rglob("*"):
        if path.is_file():
            assert b"unknown-format-fixture-secret" not in path.read_bytes(), path
    # Corruption is isolated; successful private jobs and the other user remain usable.
    bad = root / "data/by-user/user-a/memory/evolution-memory.pb"
    original = bad.read_bytes(); bad.write_bytes(b"broken fixture protobuf")
    status = learn(root, expected=3)
    assert status["shared_quarantined_users"] == 1 and status["shared_pending_users"] == 0, status
    assert not read(root / "data/memory/evolution-memory.json").get("experiences")
    bad.write_bytes(original)
    status = learn(root)
    assert status["shared_quarantined_users"] == 0


def training_does_not_vote_as_real_users(port):
    root = fixture("training-evidence")
    for user in ("user-a", "user-b"):
        for n in range(2):
            write(root / "input" / f"{user}-{n}.trace.json", trace(user, f"training-{user}-{n}"))
    learn(root, root / "input")
    for path in (root / "data/runs").glob("*.json"):
        run = read(path)
        run["training_source"] = {"kind": "baseline_training"}
        write(path, run)
    for user in ("user-a", "user-b"):
        seed_verified_memory(root, user)
    model_config(root, port)
    before = Model.calls
    learn(root)
    assert Model.calls == before, "simulated users reached real-user public promotion"
    assert not read(root / "data/memory/evolution-memory.json").get("experiences")


def echo_and_legacy(port):
    root = fixture("echo-evidence")
    for user in ("user-a", "user-b"):
        for n in range(2):
            value = trace(user, f"echo-{user}-{n}")
            value["events"][0]["input"] = "带有 trajectory-experience 注入的执行，不是独立推广证据"
            write(root / "input" / f"{user}-{n}.trace.json", value)
    learn(root, root / "input")
    for user in ("user-a", "user-b"):
        seed_verified_memory(root, user)
    model_config(root, port)
    before = Model.calls
    learn(root)
    assert Model.calls == before, "echo evidence reached the public promotion model"
    assert not read(root / "data/memory/evolution-memory.json").get("experiences")

    legacy = fixture("agentflow-sessionlog")
    rows = [dict(type="dialogue.header", version=2, dialogue_id="legacy-dialogue", conversation_id="legacy-conversation", user_id="user-a")]
    events = [("dialogue.start", 0, {"input": "你好", "trigger_type": "user"}),
              ("coordinator.activation", 0, {"activation_index": 1}),
              ("llm.request", 1, {"task_type": "coordinator", "activation_index": 1, "messages": [{"role": "user", "content": "你好"}]}),
              ("llm.response", 1, {"response": {"choices": [{"finish_reason": "stop", "message": {"content": "你好"}}]}}),
              ("loop.control", 1, {"scope": "coordinator", "action": "stop", "reason": "final_response"}),
              ("dialogue.end", 0, {"status": "normal", "response_text": "你好"})]
    for index, (kind, iteration, data) in enumerate(events, 1):
        row = dict(schema_version=2, event_id=f"coord:{index}", idempotency_key=f"coord:{index}", type=kind,
                   producer_id="coord", producer_sequence=index, timestamp_us=1000 + index,
                   dialogue_id="legacy-dialogue", node_id="coordinator_decide", data=data)
        if iteration:
            row["round"] = iteration
        rows.append(row)
    path = legacy / "input.jsonl"
    path.write_text("\n".join(json.dumps(row, ensure_ascii=False) for row in rows) + "\n")
    result = learn(legacy, path)
    assert result["imported"] == 1 and result["run_outcomes"].get("completed") == 1, result
    assert learn(legacy, path)["reused"] == 1
    assert len(list((legacy / "data/by-user/user-a/traces").rglob("*.jsonl"))) == 1


MODEL = ThreadingHTTPServer(("127.0.0.1", 0), Model)
thread = threading.Thread(target=MODEL.serve_forever, daemon=True)
thread.start()
try:
    root, logs = import_and_share(MODEL.server_port)
    rejection_and_recovery(root, logs)
    echo_and_legacy(MODEL.server_port)
    training_does_not_vote_as_real_users(MODEL.server_port)
    print("Unified learning passed: two users, offline/online, queue/restart/dedup, verified publication, scoped status, privacy and damaged-user isolation")
    print("fixtures=" + str(ROOT))
finally:
    MODEL.shutdown()
    MODEL.server_close()
