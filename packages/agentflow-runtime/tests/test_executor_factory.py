"""ToolExecutorFactory behavior tests — verifies provider selection by env/params.

The factory reads ``AF_SANDBOX_MODE`` and ``AF_SANDBOX_ENDPOINT`` (or explicit
params) and returns the correct ToolExecutor provider:

- ``none`` / unset    → HostExecutor
- ``local`` / ``docker`` → HTTPExecutor (requires endpoint)
- ``e2b``             → E2BExecutor (requires endpoint + E2B_API_KEY)

Reference: ADR 20260724-tools-runtime-refactor.md §"Factory 选择逻辑".
"""

import pytest

from agentflow_runtime.executors.base import ToolExecutor
from agentflow_runtime.executors.factory import create_executor
from agentflow_runtime.executors.host import HostExecutor
from agentflow_runtime.executors.http import HTTPExecutor
from agentflow_runtime.executors.e2b import E2BExecutor


# Valid e2b endpoint format (sandbox_id is extracted from /sandbox/{uuid}/)
E2B_ENDPOINT = "https://218.245.96.148:443/sandbox/a1b2c3d4-e5f6-7890-abcd-ef1234567890/49983"
LOCAL_ENDPOINT = "http://localhost:9999"


# ---------------------------------------------------------------------------
# none / unset mode → HostExecutor
# ---------------------------------------------------------------------------

def test_none_mode_returns_host_executor(monkeypatch):
    """AF_SANDBOX_MODE unset → HostExecutor (backward-compatible default)."""
    monkeypatch.delenv("AF_SANDBOX_MODE", raising=False)
    monkeypatch.delenv("AF_SANDBOX_ENDPOINT", raising=False)

    executor = create_executor()

    assert isinstance(executor, HostExecutor)


def test_none_mode_explicit_returns_host(monkeypatch):
    """AF_SANDBOX_MODE=none → HostExecutor."""
    monkeypatch.setenv("AF_SANDBOX_MODE", "none")
    monkeypatch.delenv("AF_SANDBOX_ENDPOINT", raising=False)

    executor = create_executor()

    assert isinstance(executor, HostExecutor)


# ---------------------------------------------------------------------------
# local / docker mode → HTTPExecutor
# ---------------------------------------------------------------------------

def test_local_mode_returns_http_executor(monkeypatch):
    """AF_SANDBOX_MODE=local + endpoint set → HTTPExecutor."""
    monkeypatch.setenv("AF_SANDBOX_MODE", "local")
    monkeypatch.setenv("AF_SANDBOX_ENDPOINT", LOCAL_ENDPOINT)

    executor = create_executor()

    assert isinstance(executor, HTTPExecutor)


def test_docker_mode_returns_http_executor(monkeypatch):
    """AF_SANDBOX_MODE=docker + endpoint set → HTTPExecutor."""
    monkeypatch.setenv("AF_SANDBOX_MODE", "docker")
    monkeypatch.setenv("AF_SANDBOX_ENDPOINT", LOCAL_ENDPOINT)

    executor = create_executor()

    assert isinstance(executor, HTTPExecutor)


# ---------------------------------------------------------------------------
# e2b mode → E2BExecutor
# ---------------------------------------------------------------------------

def test_e2b_mode_returns_e2b_executor(monkeypatch):
    """AF_SANDBOX_MODE=e2b + endpoint + E2B_API_KEY → E2BExecutor."""
    monkeypatch.setenv("AF_SANDBOX_MODE", "e2b")
    monkeypatch.setenv("AF_SANDBOX_ENDPOINT", E2B_ENDPOINT)
    monkeypatch.setenv("E2B_API_KEY", "test-key-12345")

    executor = create_executor()

    assert isinstance(executor, E2BExecutor)


# ---------------------------------------------------------------------------
# Missing endpoint → RuntimeError
# ---------------------------------------------------------------------------

def test_local_mode_no_endpoint_raises(monkeypatch):
    """AF_SANDBOX_MODE=local but endpoint empty → RuntimeError."""
    monkeypatch.setenv("AF_SANDBOX_MODE", "local")
    monkeypatch.delenv("AF_SANDBOX_ENDPOINT", raising=False)

    with pytest.raises(RuntimeError, match="local mode but endpoint empty"):
        create_executor()


def test_e2b_mode_no_endpoint_raises(monkeypatch):
    """AF_SANDBOX_MODE=e2b but endpoint empty → RuntimeError."""
    monkeypatch.setenv("AF_SANDBOX_MODE", "e2b")
    monkeypatch.delenv("AF_SANDBOX_ENDPOINT", raising=False)
    monkeypatch.setenv("E2B_API_KEY", "test-key")

    with pytest.raises(RuntimeError, match="e2b mode but endpoint empty"):
        create_executor()


# ---------------------------------------------------------------------------
# Explicit params override environment
# ---------------------------------------------------------------------------

def test_explicit_params_override_env(monkeypatch):
    """Explicit params take precedence over environment variables."""
    # Env says local+endpoint, but explicit params say none
    monkeypatch.setenv("AF_SANDBOX_MODE", "local")
    monkeypatch.setenv("AF_SANDBOX_ENDPOINT", LOCAL_ENDPOINT)

    executor = create_executor(sandbox_mode="none")

    assert isinstance(executor, HostExecutor)


# ---------------------------------------------------------------------------
# Interface contract — all returns are ToolExecutor
# ---------------------------------------------------------------------------

@pytest.mark.parametrize(
    "mode,endpoint,extra_env,expected_type",
    [
        ("none", "", {}, HostExecutor),
        ("local", LOCAL_ENDPOINT, {}, HTTPExecutor),
        ("docker", LOCAL_ENDPOINT, {}, HTTPExecutor),
        ("e2b", E2B_ENDPOINT, {"E2B_API_KEY": "k"}, E2BExecutor),
    ],
)
def test_returns_tool_executor_interface(
    monkeypatch, mode, endpoint, extra_env, expected_type
):
    """Every provider returned by the factory is a ToolExecutor subclass."""
    monkeypatch.setenv("AF_SANDBOX_MODE", mode)
    if endpoint:
        monkeypatch.setenv("AF_SANDBOX_ENDPOINT", endpoint)
    else:
        monkeypatch.delenv("AF_SANDBOX_ENDPOINT", raising=False)
    for k, v in extra_env.items():
        monkeypatch.setenv(k, v)

    executor = create_executor()

    assert isinstance(executor, ToolExecutor)
    assert isinstance(executor, expected_type)


# ---------------------------------------------------------------------------
# sandbox_workspace_prefix — 沙箱 workspace 前缀（架构修复 W1 收敛）
# ---------------------------------------------------------------------------

def test_sandbox_workspace_prefix_e2b_mode(monkeypatch):
    """e2b 模式返回沙箱 workspace 前缀(默认 /home/user/workspace)。

    架构修复 W1(2026-08-14):沙箱环境读取从 skill_executor 渗漏收敛到 executors
    factory 层——与 create_executor 同层读取 AF_SANDBOX_MODE,消除编排层
    方法名泄漏底层实现(sandbox)的渗漏抽象。
    """
    from agentflow_runtime.executors.factory import sandbox_workspace_prefix

    monkeypatch.setenv("AF_SANDBOX_MODE", "e2b")
    monkeypatch.delenv("AF_SANDBOX_WORKSPACE", raising=False)
    assert sandbox_workspace_prefix() == "/home/user/workspace"

    monkeypatch.setenv("AF_SANDBOX_WORKSPACE", "/custom/ws")
    assert sandbox_workspace_prefix() == "/custom/ws"


def test_sandbox_workspace_prefix_non_e2b_returns_empty(monkeypatch):
    """非 e2b 模式返回空串(无沙箱 workspace 语义)。"""
    from agentflow_runtime.executors.factory import sandbox_workspace_prefix

    monkeypatch.setenv("AF_SANDBOX_MODE", "local")
    assert sandbox_workspace_prefix() == ""

    monkeypatch.delenv("AF_SANDBOX_MODE", raising=False)
    assert sandbox_workspace_prefix() == ""
