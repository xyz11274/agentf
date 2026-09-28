# test_rust_local_client 回归验证：检查包内协议、边界条件与失败处理。
# 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
# 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

"""让 Rust 类型化本地客户端调用真实进程内 C 接口。"""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

SOURCE = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv[1]).resolve()
PROBE = Path(sys.argv[2]).resolve()
ROOT = Path(tempfile.mkdtemp(prefix="rust-local-client.", dir=BUILD))
TOKEN_A = "fixture-principal-alpha-00000001"
TOKEN_B = "fixture-principal-bravo-00000002"
ENV = dict(os.environ, INTEGRATION_TEST_A=TOKEN_A, INTEGRATION_TEST_B=TOKEN_B)


# 回归用例：main。
# 重点核验字段或诊断：test_integration_service、rust_local_client；异常路径不能被当成成功。
def main():
    subprocess.run(
        [str(BUILD / "test_integration_service"), "--fixture", str(ROOT)],
        check=True,
        capture_output=True,
        timeout=20,
        env=ENV,
    )
    shutil.copytree(SOURCE / "config", ROOT / "config")
    path = ROOT / "service.json"
    config = json.loads(path.read_text(encoding="utf-8"))
    config["runtime_config"] = str(ROOT / "config/standard-runtime.json")
    config["auto_evaluate"] = False
    config["runtime_options"]["loop_rules"] = []
    path.write_text(json.dumps(config), encoding="utf-8")
    completed = subprocess.run(
        [str(PROBE), str(BUILD / "libtrajectory_evolution_local.so"), str(path)],
        check=True,
        capture_output=True,
        text=True,
        timeout=30,
        env=ENV,
    )
    result = json.loads(completed.stdout)
    assert result["ok"] is True and result["transport"] == "local.native.v1", result
    for artifact in (ROOT / "data").rglob("*"):
        if artifact.is_file():
            content = artifact.read_bytes()
            assert (
                TOKEN_A.encode() not in content and TOKEN_B.encode() not in content
            ), artifact
    print(json.dumps({"rust_local_client": result, "output": str(ROOT)}))


if __name__ == "__main__":
    main()
