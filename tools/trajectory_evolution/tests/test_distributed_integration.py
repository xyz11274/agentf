"""Independent producer, concurrent receivers and later callback process over real HTTP."""
import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import sys
import tempfile
import time

SOURCE = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv[1]).resolve()
ROOT = Path(tempfile.mkdtemp(prefix="distributed-integration.", dir=BUILD))
TOKEN = "fixture-principal-alpha-00000001"
ENV = dict(os.environ, INTEGRATION_TEST_A=TOKEN, INTEGRATION_TEST_B="fixture-principal-bravo-00000002",
           EVOLUTION_TOKEN=TOKEN, PYTHONDONTWRITEBYTECODE="1")
SCRIPT = SOURCE / "examples/distributed-agent/participant.py"


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def main():
    subprocess.run([str(BUILD / "test_integration_service"), "--fixture", str(ROOT)], env=ENV, check=True, capture_output=True)
    config_path = ROOT / "service.json"
    config = json.loads(config_path.read_text())
    config["auto_evaluate"] = False
    config["runtime_options"]["request_context_pointer"] = "/messages"
    config["runtime_options"]["request_context_mode"] = "system_message"
    config_path.write_text(json.dumps(config))
    with (ROOT / "server.stderr").open("w") as errors:
        server = subprocess.Popen([str(BUILD / "trajectory_evolution"), "serve", "--config", str(config_path)],
            env=ENV, stdout=subprocess.PIPE, stderr=errors, text=True)
        children = []
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(server.stdout, selectors.EVENT_READ)
                assert selector.select(15), "server not ready"
            port = re.search(r"listening=127.0.0.1:(\d+)", server.stdout.readline()).group(1)
            env = dict(ENV, EVOLUTION_URL="http://127.0.0.1:" + port)
            for mode in ["execution", "delivery"]:
                mailbox = ROOT / (mode + ".sqlite")
                def command(role, *args):
                    return [sys.executable, "-B", str(SCRIPT), role, str(mailbox), *args]
                sender = subprocess.run(command("send", "--level", "L3", "--completion", mode),
                    env=env, check=True, capture_output=True, text=True, timeout=20)
                run = json.loads(sender.stdout)["run_id"]
                # Sender has exited. No object or Python process is retained to run the children.
                children = [subprocess.Popen(command("receive", "--job", "job-a", "--barrier", "--wait-for", "job-b"),
                    env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True),
                    subprocess.Popen(command("receive", "--job", "job-b", "--barrier", "--fail"),
                    env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)]
                for process in children:
                    stdout, stderr = process.communicate(timeout=25)
                    assert process.returncode == 0, stderr
                    assert json.loads(stdout)["status"] in ["completed", "failed"]
                collector = subprocess.run(command("collect"), env=env, check=True, capture_output=True, text=True, timeout=20)
                assert json.loads(collector.stdout)["capture_complete"]
                trace_path = ROOT / "data/by-user/user-a/traces/distributed-demo" / (run + ".trace.json")
                for _ in range(100):
                    if trace_path.exists():
                        break
                    time.sleep(0.03)
                trace = json.loads(trace_path.read_text())
                events = trace["events"]
                starts = {e["execution_id"]: i for i, e in enumerate(events) if e["kind"] == "step.start" and e["execution_id"].startswith("job-")}
                ends = {e["execution_id"]: i for i, e in enumerate(events) if e.get("attributes", {}).get("type") == "execution.end"}
                assert max(starts.values()) < min(ends.values()), "receivers did not overlap"
                assert ends["job-b"] < ends["job-a"], "reverse completion was not exercised"
                assert events[-1]["outcome"] == {"execution_status": "completed", "business_status": "partial"}
                checked = subprocess.run([str(BUILD / "trajectory_evolution"), "doctor", str(trace_path), "--level", "L0"],
                    capture_output=True, text=True, timeout=15)
                assert checked.returncode == 0, checked.stderr
        finally:
            for process in children:
                stop(process)
            stop(server)
    for file in ROOT.rglob("*"):
        if file.is_file():
            assert TOKEN.encode() not in file.read_bytes(), "service credential persisted"
    print(json.dumps({"execution_and_delivery": "passed", "separate_processes": True,
        "overlapping_receivers": True, "reverse_completion": True, "output": str(ROOT)}))


if __name__ == "__main__":
    main()
