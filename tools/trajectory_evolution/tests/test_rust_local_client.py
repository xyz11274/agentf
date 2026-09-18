"""Run the Rust typed local client against the real in-process C ABI."""
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
            assert TOKEN_A.encode() not in content and TOKEN_B.encode() not in content, artifact
    print(json.dumps({"rust_local_client": result, "output": str(ROOT)}))


if __name__ == "__main__":
    main()
