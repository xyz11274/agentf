"""T032 bash.py 改造后集成测试——sandbox-mode=local 下 echo hello 返回正确、rm -rf / 被拒绝。

覆盖 AC-US1-002，引用 TC-SBOX-001（echo 返回正确）+ TC-SBOX-020（rm -rf 拒绝）+ TC-SBOX-021（sudo/fork bomb 拒绝）。

运行方式：pytest packages/agentflow-runtime/tests/integration/test_bash_sandbox_integration.py -v
"""
import json
import sys
from pathlib import Path

import pytest

# 把 tools 目录加入 sys.path 以直接导入 bash.py（绕过 skill_executor 动态加载）
_TOOLS_DIR = Path(__file__).resolve().parents[2] / "src" / "agentflow_runtime" / "harness" / "tools"
sys.path.insert(0, str(_TOOLS_DIR))
from bash import execute, _is_dangerous  # noqa: E402


# TC-SBOX-001: echo hello 在 sandbox-mode=none（向后兼容路径）下返回正确
def test_echo_returns_correct_backward_compat(monkeypatch):
    """TC-SBOX-001: 显式 unsafe opt-in 后宿主兼容路径返回正确。

    业务规则: B1 默认隔离；仅显式 unsafe opt-in 验证旧宿主执行兼容性
    错误模型: 命令泄漏——bash.py 改造后向后兼容路径断裂
    失败意味着: sandbox-mode=none 回归失败，现有部署不兼容
    """
    monkeypatch.setenv("AF_ALLOW_UNSAFE_LOCAL_SECRET_TOOLS", "1")
    result = json.loads(execute({"command": "echo hello_backward_compat", "timeout": 5}, sandbox_endpoint=None))
    assert result.get("stdout", "").strip() == "hello_backward_compat", f"echo 返回错误: {result}"
    assert result.get("exit_code") == 0, f"exit_code 非 0: {result}"


# TC-SBOX-020: rm -rf / 宿主侧拒绝含命令名
def test_dangerous_command_rejected():
    """TC-SBOX-020: rm -rf / 宿主侧第一道防线拒绝含命令名。

    业务规则: B2 危険命令前缀在宿主侧第一道防线即拒绝
    错误模型: 危険命令泄漏——检查遗漏或顺序错误
    失败意味着: rm -rf / 在沙箱内执行，可能误删沙箱内文件甚至逃逸到宿主机
    """
    result = json.loads(execute({"command": "rm -rf /", "timeout": 5}, sandbox_endpoint=None))
    assert "error" in result, f"危険命令未被拒绝: {result}"
    assert "Dangerous command rejected" in result["error"], f"拒绝信息不含命令名: {result}"
    assert "rm -rf /" in result["error"], f"拒绝信息不含具体命令: {result}"


# TC-SBOX-021: sudo/fork bomb 伪装命中拒绝
def test_sudo_forkbomb_rejected():
    """TC-SBOX-021: sudo/fork bomb 伪装命中拒绝。

    业务规则: B2 危険命令前缀拒绝（sudo / fork bomb）
    错误模型: 危険命令泄漏——伪装形式未命中检查前缀
    失败意味着: sudo 命令在沙箱内执行提权，或 fork bomb 耗尽沙箱资源
    """
    # sudo 前缀
    result_sudo = json.loads(execute({"command": "sudo rm something", "timeout": 5}, sandbox_endpoint=None))
    assert "Dangerous command rejected" in result_sudo.get("error", ""), f"sudo 未拒绝: {result_sudo}"
    # fork bomb 前缀
    result_fb = json.loads(execute({"command": ":(){ :|:& };:", "timeout": 5}, sandbox_endpoint=None))
    assert "Dangerous command rejected" in result_fb.get("error", ""), f"fork bomb 未拒绝: {result_fb}"


# 额外：空命令拒绝
def test_empty_command_rejected():
    """空命令拒绝含可定位原因。"""
    result = json.loads(execute({"command": "   ", "timeout": 5}, sandbox_endpoint=None))
    assert result.get("error") == "Empty command", f"空命令未拒绝: {result}"


# 额外：_is_dangerous 单元测试——覆盖所有危険前缀
@pytest.mark.parametrize("cmd, expected", [
    ("rm -rf /etc", True),
    ("sudo apt update", True),
    (":(){ :|:& };:", True),
    ("echo hello", False),
    ("ls -la", False),
    ("git status", False),
])
def test_is_dangerous_parametrized(cmd, expected):
    """_is_dangerous 覆盖所有危険前缀 + 安全命令。"""
    assert _is_dangerous(cmd) == expected, f"_is_dangerous('{cmd}') = {_is_dangerous(cmd)}, 预期 {expected}"
