# test_boundary_transport 回归验证：检查包内协议、边界条件与失败处理。
# 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
# 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

"""验证同一运行跨 C++ 生产者及独立 Python、JavaScript 接收者传递，不依赖 Agent 框架。"""

from concurrent.futures import ThreadPoolExecutor
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
import uuid

SOURCE = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv[1]).resolve()
ROOT = Path(tempfile.mkdtemp(prefix="boundary-transport.", dir=BUILD))
TOKEN = "fixture-principal-alpha-00000001"
ENV = dict(
    os.environ,
    INTEGRATION_TEST_A=TOKEN,
    INTEGRATION_TEST_B="fixture-principal-bravo-00000002",
    EVOLUTION_TEST_TOKEN=TOKEN,
    PYTHONDONTWRITEBYTECODE="1",
)


# 回归用例：main。
# 重点核验字段或诊断：test_integration_service、test_client_cpp、producer_processes_exited；异常路径不能被当成成功。
def main():
    subprocess.run(
        [str(BUILD / "test_integration_service"), "--fixture", str(ROOT)],
        env=ENV,
        check=True,
        capture_output=True,
    )
    config_path = ROOT / "service.json"
    config = json.loads(config_path.read_text())
    config["auto_evaluate"] = False
    config_path.write_text(json.dumps(config))
    commands = {
        "cpp": [str(BUILD / "test_client_cpp")],
        "python": [sys.executable, "-B", str(SOURCE / "tests/clients/probe.py")],
    }
    if shutil.which("node"):
        commands["javascript"] = [
            shutil.which("node"),
            str(SOURCE / "tests/clients/probe.mjs"),
        ]
    receiver_languages = ["python", "javascript" if "javascript" in commands else "cpp"]
    with (ROOT / "service.stderr").open("w") as errors:
        server = subprocess.Popen(
            [
                str(BUILD / "trajectory_evolution"),
                "serve",
                "--config",
                str(config_path),
            ],
            env=ENV,
            stdout=subprocess.PIPE,
            stderr=errors,
            text=True,
        )
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(server.stdout, selectors.EVENT_READ)
                assert selector.select(15), "service startup timeout"
            port = re.search(
                r"listening=127.0.0.1:(\d+)", server.stdout.readline()
            ).group(1)
            env = dict(ENV, EVOLUTION_URL="http://127.0.0.1:" + port)

            # 测试辅助函数：rpc。
            # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
            def rpc(language, operation, fields):
                message = dict(fields, operation=operation, request_id=uuid.uuid4().hex)
                # 每个原生进程完成协议请求后退出，只有显式 JSON 上下文跨进程保留。
                result = subprocess.run(
                    commands[language],
                    input=json.dumps(message, ensure_ascii=False) + "\n",
                    env=env,
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    timeout=15,
                )
                assert result.returncode == 0, result.stderr
                response = json.loads(result.stdout)
                assert response["ok"], (operation, response)
                return response["result"]

            run = rpc(
                "cpp",
                "run.begin",
                {
                    "input": "视频生成缺少素材",
                    "level": "L3",
                    "capabilities": ["model", "actions", "loop"],
                },
            )
            common = {"run_id": run["run_id"]}
            jobs = [
                rpc(
                    "cpp",
                    "activity.begin",
                    dict(common, kind="custom", name="remote-job", input={"number": i}),
                )
                for i in range(2)
            ]
            mailbox = ROOT / "queued-contexts.json"
            mailbox.write_text(json.dumps([job["context"] for job in jobs]))
            received = json.loads(mailbox.read_text())
            barrier = threading.Barrier(2, timeout=15)
            fast_finished = threading.Event()

            # 测试辅助函数：receive。
            # 为当前回归场景构造输入或读取结果，调用参数及返回值由下方断言约束。
            def receive(index):
                language = receiver_languages[index]
                context = received[index]
                control = rpc(
                    language,
                    "loop.check",
                    dict(
                        common,
                        loop_id="independent-" + str(index),
                        state={"ready": True},
                    ),
                )
                rpc(
                    language,
                    "loop.commit",
                    dict(
                        common,
                        control_id=control["control_id"],
                        decision=control["decision"],
                    ),
                )
                model = rpc(
                    language,
                    "activity.begin",
                    dict(
                        common,
                        kind="model",
                        name="native-request",
                        input={"system": "原始指令"},
                        binding={"pointer": "/system", "mode": "text"},
                        control_id=control["control_id"],
                        actor_id="dynamic-" + language,
                        caused_by=[jobs[index]["event_id"]],
                    ),
                )
                assert "先补充图片" in model["input"]["system"]
                barrier.wait()
                if index == 0:
                    assert fast_finished.wait(15), "fast branch did not finish"
                outcome = "failed" if index else "completed"
                end = rpc(
                    language,
                    "activity.end",
                    dict(
                        common,
                        activity_id=model["activity_id"],
                        actual_input=model["input"],
                        status=outcome,
                        executed=True,
                        output={"worker": language},
                    ),
                )
                result = rpc(
                    language,
                    "activity.end",
                    dict(
                        context,
                        status=outcome,
                        output={"worker": language},
                        caused_by=[end["event_id"]],
                    ),
                )
                if index:
                    fast_finished.set()
                return result

            with ThreadPoolExecutor(max_workers=2) as pool:
                results = list(pool.map(receive, range(2)))
            join = rpc(
                "cpp",
                "activity.begin",
                dict(
                    common,
                    kind="merge",
                    name="final-join",
                    input="merge",
                    caused_by=[result["event_id"] for result in results],
                ),
            )
            rpc(
                "cpp",
                "activity.end",
                dict(join["context"], status="completed", output="部分完成"),
            )
            rpc(
                "cpp",
                "run.end",
                dict(
                    common,
                    status="completed",
                    business_status="partial",
                    output="一个分支失败，另一个成功",
                ),
            )
            path = (
                ROOT
                / "data/by-user/user-a/traces"
                / run["run_id"]
                / (run["run_id"] + ".trace.json")
            )
            for _ in range(100):
                if path.exists():
                    break
                time.sleep(0.03)
            trace = json.loads(path.read_text())
            events = trace["events"]
            model_starts = [
                i for i, e in enumerate(events) if e["kind"] == "action.start"
            ]
            model_ends = [i for i, e in enumerate(events) if e["kind"] == "action.end"]
            assert len(model_starts) == 2 and max(model_starts) < min(model_ends)
            assert (
                events[model_ends[0]]["status"] == "failed"
                and events[model_ends[1]]["status"] == "completed"
            )
            checked = subprocess.run(
                [
                    str(BUILD / "trajectory_evolution"),
                    "doctor",
                    str(path),
                    "--level",
                    "L0",
                ],
                capture_output=True,
                text=True,
                timeout=15,
            )
            assert checked.returncode == 0, checked.stderr
            summary = {
                "protocol": run["protocol"],
                "languages": list(commands),
                "producer_processes_exited": True,
                "concurrent_receivers": True,
                "reverse_completion": True,
                "valid_trace": str(path),
                "output": str(ROOT),
            }
            (ROOT / "test-summary.json").write_text(json.dumps(summary, indent=2))
            print(json.dumps(summary))
        finally:
            server.terminate()
            try:
                server.wait(timeout=8)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=3)
    for path in ROOT.rglob("*"):
        if path.is_file():
            assert TOKEN.encode() not in path.read_bytes(), "credential persisted"


if __name__ == "__main__":
    main()
