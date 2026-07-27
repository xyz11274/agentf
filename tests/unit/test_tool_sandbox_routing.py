"""验证 bash 工具通过 ToolExecutor 抽象层的路由行为。

Phase 4 更新：测试目标从 outfit 副本 bash.py 改为 runtime 包 bash.py。
路由决策由 ``create_executor()`` factory 读取 ``AF_SANDBOX_MODE`` 决定：
- ``none`` / 未设置 → HostExecutor（宿主直接执行，忽略 endpoint）
- ``local`` / ``docker`` → HTTPExecutor（需要非空 endpoint）
- ``e2b`` → E2BExecutor（需要非空 endpoint + E2B_API_KEY）

参考: ADR 20260724-tools-runtime-refactor.md §"Factory 选择逻辑"。
"""
import json
import os
import sys
from pathlib import Path

import pytest

# 被测试的模块（runtime 包 bash.py，Phase 4 后 outfit 副本已删除）
_TOOLS_SRC = (
    Path(__file__).resolve().parents[2]
    / "packages"
    / "agentflow-runtime"
    / "src"
    / "agentflow_runtime"
    / "harness"
    / "tools"
)
_BASH_PY = _TOOLS_SRC / "bash.py"
assert _BASH_PY.is_file(), f"not found: {_BASH_PY}"

# 确保 runtime 包 src 在 sys.path（factory.py 等 import agentflow_runtime.*）
_RUNTIME_SRC = _TOOLS_SRC.parents[2]  # .../agentflow-runtime/src
if str(_RUNTIME_SRC) not in sys.path:
    sys.path.insert(0, str(_RUNTIME_SRC))


def _import_bash():
    """动态导入 runtime bash 模块（隔离测试，不影响全局）。"""
    import importlib.util

    spec = importlib.util.spec_from_file_location("_test_bash_tool", str(_BASH_PY))
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestBashToolSandboxRouting:
    """bash 工具 ToolExecutor 路由决策测试。"""

    @pytest.fixture(autouse=True)
    def _clean_env(self):
        """每个测试前清理环境变量。"""
        old = {}
        for k in ("AF_SANDBOX_MODE", "AF_SANDBOX_ENDPOINT", "AF_ARTIFACT_STORAGE_URL"):
            old[k] = os.environ.pop(k, None)
        yield
        for k, v in old.items():
            if v is not None:
                os.environ[k] = v

    # ── mode=none（默认）+ endpoint 非空 → HostExecutor（endpoint 被忽略）──
    def test_routes_to_host_when_mode_none_even_with_endpoint(self):
        """mode=none 时即使 endpoint 非空也走 HostExecutor（本地执行）。

        Factory 只看 AF_SANDBOX_MODE 决定 provider，endpoint 在 mode=none
        时被忽略——这是与旧版（看 endpoint 决定路由）的关键行为差异。
        """
        bash = _import_bash()
        result = bash.execute(
            {"command": "echo hello_host"},
            sandbox_endpoint="http://sandbox:8080",
        )
        data = json.loads(result)
        assert data.get("stdout", "").strip() == "hello_host", (
            f"mode=none 应走本地，但得到: {result}"
        )
        assert data.get("exit_code") == 0

    # ── mode=none + endpoint 空 → HostExecutor（本地）──────────────────
    def test_routes_to_host_when_endpoint_unset(self):
        """mode=none + endpoint 为空时走本地 subprocess。"""
        bash = _import_bash()
        result = bash.execute({"command": "echo ok_local"}, sandbox_endpoint="")
        data = json.loads(result)
        assert data.get("stdout", "").strip() == "ok_local", f"期望本地执行: {result}"
        assert data.get("exit_code") == 0

    # ── mode=e2b + endpoint 空 → RuntimeError（factory guard）──────────
    def test_fails_when_e2b_mode_but_no_endpoint(self):
        """AF_SANDBOX_MODE=e2b 但 endpoint 为空时抛 RuntimeError。"""
        os.environ["AF_SANDBOX_MODE"] = "e2b"
        bash = _import_bash()
        with pytest.raises(RuntimeError, match="e2b mode but endpoint empty"):
            bash.execute({"command": "echo should_not_run"}, sandbox_endpoint="")

    # ── mode=local + endpoint 空 → RuntimeError（factory guard）────────
    def test_fails_when_local_mode_but_no_endpoint(self):
        """AF_SANDBOX_MODE=local 但 endpoint 为空时抛 RuntimeError。"""
        os.environ["AF_SANDBOX_MODE"] = "local"
        bash = _import_bash()
        with pytest.raises(RuntimeError, match="local mode but endpoint empty"):
            bash.execute({"command": "echo should_not_run"}, sandbox_endpoint="")

    # ── mode=e2b + endpoint 非空 → E2BExecutor 被选中（构造时验证）────────
    def test_routes_to_e2b_when_mode_e2b_with_endpoint(self):
        """mode=e2b + endpoint 格式正确时 Factory 选择 E2BExecutor。

        E2BExecutor 构造函数解析 endpoint 中的 sandbox_id 并检查 E2B_API_KEY。
        不设 E2B_API_KEY 时抛 RuntimeError——证明 E2BExecutor 被选中（而非
        HostExecutor，后者不会检查任何 API key）。
        """
        os.environ["AF_SANDBOX_MODE"] = "e2b"
        os.environ.pop("E2B_API_KEY", None)  # 确保未设
        bash = _import_bash()
        # endpoint 必须匹配 /sandbox/{hex-uuid}/{port} 格式
        valid_endpoint = "https://218.245.96.148:443/sandbox/abc123def456/49983"
        with pytest.raises(RuntimeError, match="E2B_API_KEY not set"):
            bash.execute(
                {"command": "echo hello"},
                sandbox_endpoint=valid_endpoint,
            )

    # ── 边界：空命令 ───────────────────────────────────────────────────
    def test_empty_command_returns_error(self):
        bash = _import_bash()
        result = bash.execute({"command": ""})
        data = json.loads(result)
        assert "Empty command" in data.get("error", "")

    # ── 边界：危险命令被宿主侧第一道防线拒绝 ─────────────────────────────
    def test_dangerous_command_rejected(self):
        bash = _import_bash()
        result = bash.execute({"command": "rm -rf /"})
        data = json.loads(result)
        assert "Dangerous command rejected" in data.get("error", "")
