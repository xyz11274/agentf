# test_installation 回归验证：检查包内协议、边界条件与失败处理。
# 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
# 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

"""在没有 AgentFlow 的环境验证已安装的二进制、配置和 Python 客户端。"""

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
import time
import uuid

prefix = Path(sys.argv[1]).resolve()
root = Path(
    tempfile.mkdtemp(prefix="installed-service.", dir=Path(sys.argv[2]).resolve())
)
share = prefix / "share/trajectory_evolution"
shutil.copytree(share / "config", root / "config")
config_path = root / "config/boundary-service.json"
config = json.loads(config_path.read_text(encoding="utf-8"))
config.update(
    data_dir=str(root / "data"),
    port=0,
    principals=[{"user_id": "install-user", "token_env": "INSTALL_TEST_TOKEN"}],
)
config["learning"].update(min_new_runs=1, min_interval_ms=0)
config_path.write_text(json.dumps(config), encoding="utf-8")
token = uuid.uuid4().hex + uuid.uuid4().hex
env = dict(os.environ, INSTALL_TEST_TOKEN=token)
client_file = share / "clients/python/evolution.py"
spec = importlib.util.spec_from_file_location("installed_evolution_client", client_file)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with (root / "service.stderr").open("w", encoding="utf-8") as errors:
    server = subprocess.Popen(
        [
            str(prefix / "bin/trajectory_evolution"),
            "serve",
            "--config",
            str(config_path),
        ],
        stdout=subprocess.PIPE,
        stderr=errors,
        env=env,
        text=True,
        encoding="utf-8",
    )
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(server.stdout, selectors.EVENT_READ)
            assert selector.select(10), "installed service startup timed out"
        port = re.search(
            r"listening=127\.0\.0\.1:(\d+)", server.stdout.readline()
        ).group(1)
        client = module.Evolution("http://127.0.0.1:" + port, token)
        run = client.invoke(
            "run.begin",
            {
                "conversation_id": "独立安装会话",
                "input": "检查部署后的真实接口",
                "level": "L3",
                "capabilities": ["model", "actions", "loop"],
            },
        )
        common = {"run_id": run["run_id"]}
        control = client.invoke(
            "loop.check", dict(common, loop_id="install-check", state={"completed": 0})
        )
        client.invoke(
            "loop.commit",
            dict(
                common, control_id=control["control_id"], decision=control["decision"]
            ),
        )
        model = client.invoke(
            "activity.begin",
            dict(
                common,
                kind="model",
                name="check",
                input="检查已安装客户端。",
                binding={"pointer": "", "mode": "text"},
                control_id=control["control_id"],
                actor_id="unregistered-worker",
            ),
        )
        client.invoke(
            "activity.end",
            dict(
                common,
                activity_id=model["activity_id"],
                actual_input=model["input"],
                status="completed",
                executed=True,
                output="检查完成",
            ),
        )
        plan = client.invoke(
            "activity.begin", dict(common, kind="action", name="lookup", input={})
        )
        assert plan["decision"] == "allow"
        client.invoke(
            "activity.end",
            dict(
                common,
                activity_id=plan["activity_id"],
                output={"found": True},
                status="completed",
                executed=True,
            ),
        )
        client.invoke(
            "run.end",
            dict(
                common, status="completed", business_status="succeeded", output="完成"
            ),
        )
        for _ in range(200):
            status = client.invoke("run.status", common)
            assert status["evaluation_status"] != "failed", status["evaluation_error"]
            if status["evaluation_status"] == "completed":
                break
            time.sleep(0.05)
        assert status["evaluation_status"] == "completed" and status["capture_complete"]
        observed = client.point(
            {
                "phase": "before",
                "kind": "run",
                "input": "installed point",
                "level": "L0",
            },
            timeout_ms=1000,
        )
        assert not observed.get("degraded"), observed
        finished = client.point(
            {"phase": "after", "context": observed["context"], "output": "done"},
            timeout_ms=1000,
        )
        assert finished["closed"] and finished["capture_complete"]
        trace = next((root / "data/by-user/install-user/traces").rglob("*.trace.json"))
        subprocess.run(
            [
                str(prefix / "bin/trajectory_evolution"),
                "doctor",
                str(trace),
                "--level",
                "L0",
            ],
            env=env,
            check=True,
            capture_output=True,
        )
        assert (root / "data/by-user/install-user/memory/evolution-memory.pb").is_file()
        for file in (root / "data").rglob("*"):
            if file.is_file():
                assert token.encode() not in file.read_bytes(), (
                    "service token was persisted"
                )
        summary = {
            "installed_binary": str(prefix / "bin/trajectory_evolution"),
            "installed_client": str(client_file),
            "trace_valid": True,
            "automatic_evaluation": "completed",
            "memory_initialized": True,
            "provider_calls": 0,
            "output": str(root),
        }
        (root / "test-summary.json").write_text(
            json.dumps(summary, indent=2), encoding="utf-8"
        )
        print(json.dumps(summary))
    finally:
        if server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=3)
