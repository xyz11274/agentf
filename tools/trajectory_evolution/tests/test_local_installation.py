# test_local_installation 回归验证：检查包内协议、边界条件与失败处理。
# 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
# 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

"""脱离源码树加载安装后的原生动态库和客户端。"""

import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

prefix, build = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
root = Path(tempfile.mkdtemp(prefix="installed-local.", dir=build))
share = prefix / "share/trajectory_evolution"
shutil.copytree(share / "config", root / "config")
path = root / "config/boundary-service.json"
config = json.loads(path.read_text())
config.update(
    data_dir=str(root / "data"),
    principals=[{"user_id": "installed-native", "token_env": "LOCAL_INSTALL_TOKEN"}],
)
config["learning"].update(min_new_runs=1, min_interval_ms=0)
path.write_text(json.dumps(config))
os.environ["LOCAL_INSTALL_TOKEN"] = "fixture-installed-native-principal-0001"
spec = importlib.util.spec_from_file_location(
    "installed_local", share / "clients/python/evolution_local.py"
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
client = module.LocalEvolution(
    prefix / "lib/libtrajectory_evolution_local.so",
    path,
    os.environ["LOCAL_INSTALL_TOKEN"],
)
try:
    # 包装宿主业务调用：先查询策略，再最多执行一次原业务函数，最后记录结果。
    def call(op, **fields):
        result = client.invoke(op, fields)
        assert not result.get("degraded"), (op, result)
        return result

    started = client.point(
        dict(
            phase="before",
            kind="run",
            input="独立安装测试",
            level="L1",
            capabilities=["model"],
        )
    )
    assert not started.get("degraded"), started
    run = started["context"]["run_id"]
    model = client.point(
        dict(
            phase="before",
            context=started["context"],
            kind="model",
            name="reason",
            input="正常输入",
            binding={"pointer": "", "mode": "text"},
        )
    )
    assert not model.get("degraded"), model
    ended = client.point(
        dict(
            phase="after",
            context=model["context"],
            input=model["input"],
            executed=True,
            output="正常输出",
        )
    )
    assert not ended.get("degraded"), ended
    closed = client.point(
        dict(phase="after", context=started["context"], output="完成")
    )
    assert closed["capture_complete"] and closed["metrics"]["llm_calls"] == 1
    for _ in range(200):
        if call("run.status", run_id=run)["evaluation_status"] == "completed":
            break
        time.sleep(0.05)
    assert call("run.status", run_id=run)["evaluation_status"] == "completed"
    trace = next((root / "data/by-user/installed-native/traces").rglob("*.trace.json"))
    subprocess.run(
        [
            str(prefix / "bin/trajectory_evolution"),
            "doctor",
            str(trace),
            "--level",
            "L0",
        ],
        check=True,
        capture_output=True,
        timeout=20,
    )
    for file in (root / "data").rglob("*"):
        if file.is_file():
            assert os.environ["LOCAL_INSTALL_TOKEN"].encode() not in file.read_bytes()
    result = {
        "output": str(root),
        "installed_library": str(prefix / "lib/libtrajectory_evolution_local.so"),
        "trace_valid": True,
        "evaluation_completed": True,
        "paid_provider_calls": 0,
    }
    (root / "test-summary.json").write_text(json.dumps(result, indent=2))
    print(json.dumps(result))
finally:
    client.close()
