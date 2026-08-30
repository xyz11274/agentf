"""Cross-component credential boundary regression tests."""

from __future__ import annotations

import asyncio
import json
from types import SimpleNamespace
from unittest.mock import MagicMock

import pytest

from agentflow_runtime.context import AgentContext
from agentflow_runtime.databus_client import DataBusClient
from agentflow_runtime.executors.host import HostExecutor
from agentflow_runtime.harness.tools import edit_file as edit_file_tool
from agentflow_runtime.harness.tools import read_file as read_file_tool
from agentflow_runtime.harness.tools import web_fetch as web_fetch_tool
from agentflow_runtime.harness.tools import write_file as write_file_tool
from agentflow_runtime.harness.mcp.mcp_bridge import (
    MCPServerConfig,
    _AsyncMCPBridge,
)
from agentflow_runtime.hooks.engine import trigger
from agentflow_runtime.hooks.registry import HookRegistry
from agentflow_runtime.security import register_secret
from agentflow_runtime.security.secrets import _reset_registry_for_tests
from agentflow_runtime.security.trusted_media import (
    TrustedMediaError,
    materialize_media,
    publish_media,
    validate_credential_url,
)
from agentflow_runtime.tool_registry import _LocalToolRegistry


@pytest.fixture(autouse=True)
def _isolated_secret_registry():
    _reset_registry_for_tests()
    yield
    _reset_registry_for_tests()


class _Namespace:
    def __init__(self):
        self.values = {}
        self.get_value = None

    def put(self, key, value):
        self.values[key] = value
        return True

    def get(self, key, default=None):
        if self.get_value is not None:
            return self.get_value
        return self.values.get(key, default)

    def remove(self, key):
        self.values.pop(key, None)


class _DataBusBackend:
    def __init__(self):
        self.ctx_ns = _Namespace()
        self.sys_ns = _Namespace()
        self.pushes = []

    def ctx(self):
        return self.ctx_ns

    def sys(self):
        return self.sys_ns

    def response_push(self, content, **kwargs):
        self.pushes.append((content, kwargs))
        return True


def _make_databus(backend):
    bus = DataBusClient.__new__(DataBusClient)
    bus._session_id = "security-session"
    bus._dialogue_id = "security-dialogue"
    bus._client = backend
    return bus


def test_databus_sanitizes_keys_values_reads_and_stream_metadata():
    secret = "databus-secret-value-998877665544"
    register_secret("provider.databus", secret)
    backend = _DataBusBackend()
    bus = _make_databus(backend)

    assert bus.put(f"result:{secret}", json.dumps({"value": secret}))
    assert bus.put_global(f"global:{secret}", secret)
    assert bus.write_messages([{"role": "tool", "content": secret}])
    assert bus.response_push(
        secret,
        type="progress",
        source=secret,
        skill=secret,
        status=secret,
    )

    backend.sys_ns.get_value = secret
    assert secret not in str(bus.get(f"result:{secret}"))
    assert secret not in str(bus.get_global(f"global:{secret}"))

    serialized = json.dumps({
        "ctx": backend.ctx_ns.values,
        "sys": backend.sys_ns.values,
        "pushes": backend.pushes,
    })
    assert secret not in serialized
    assert "<redacted:provider.databus>" in serialized


def test_hook_sanitizes_handler_input_output_context_and_exception(caplog):
    secret = "hook-secret-value-998877665544"
    register_secret("provider.hook", secret)
    registry = HookRegistry()

    def handler(event, context):
        assert secret not in json.dumps(event.payload)
        assert secret not in json.dumps(context.messages)
        context.messages.append({"role": "system", "content": secret})
        context.artifacts.append(secret)
        context.block_reason = secret
        return {"handler_result": secret}

    def failing_handler(_event, _context):
        raise RuntimeError(f"handler failed with {secret}")

    registry.register("mutating", ["security.event"], handler, order=10)
    registry.register("failing", ["security.event"], failing_handler, order=20)
    context = AgentContext(
        messages=[{"role": "user", "content": secret}],
        artifacts=[secret],
        block_reason=secret,
    )

    result = trigger(
        "security.event",
        {"input": secret},
        ctx=context,
        registry=registry,
    )

    serialized = json.dumps({
        "result": result,
        "messages": context.messages,
        "artifacts": context.artifacts,
        "reason": context.block_reason,
        "logs": caplog.text,
    })
    assert secret not in serialized
    assert "<redacted:provider.hook>" in serialized


def _make_tool_registry(module=None):
    registry = _LocalToolRegistry.__new__(_LocalToolRegistry)
    registry._tools = []
    registry._modules = {"echo": module} if module is not None else {}
    registry._mcp_bridges = {}
    return registry


def test_tool_registry_blocks_secret_arguments_and_redacts_results(monkeypatch):
    secret = "tool-secret-value-998877665544"
    monkeypatch.setenv("ODD_VENDOR_TOKEN", secret)
    module = SimpleNamespace(
        execute=lambda _args, sandbox_endpoint=None: {
            "status": "ok",
            "result": secret,
        },
    )
    registry = _make_tool_registry(module)

    blocked = json.loads(registry.execute("echo", {"value": secret}))
    assert blocked["status"] == "security_policy_blocked"
    assert secret not in json.dumps(blocked)

    result = json.loads(registry.execute("echo", {"value": "safe"}))
    assert secret not in json.dumps(result)
    assert "<redacted:env.ODD_VENDOR_TOKEN>" in result["result"]


def test_host_executor_never_starts_command_without_explicit_opt_in(monkeypatch):
    monkeypatch.delenv("AF_ALLOW_UNSAFE_LOCAL_SECRET_TOOLS", raising=False)
    monkeypatch.setattr(
        "agentflow_runtime.executors.host.subprocess.run",
        lambda *_args, **_kwargs: pytest.fail("subprocess must not start"),
    )

    result = HostExecutor().exec_command("env")

    assert result.exit_code == -1
    assert result.error.startswith("Host command blocked")


def test_host_executor_never_starts_command_when_credentials_are_loaded(
    monkeypatch,
):
    secret = "host-secret-value-998877665544"
    monkeypatch.setenv("ODD_VENDOR_TOKEN", secret)
    monkeypatch.delenv("AF_ALLOW_UNSAFE_LOCAL_SECRET_TOOLS", raising=False)
    monkeypatch.setattr(
        "agentflow_runtime.executors.host.subprocess.run",
        lambda *_args, **_kwargs: pytest.fail("subprocess must not start"),
    )

    result = HostExecutor().exec_command("env")

    assert result.exit_code == -1
    assert result.error == "Host command blocked while credentials are loaded"


def test_host_file_tools_reject_sibling_prefix_and_symlink(monkeypatch, tmp_path):
    workspace = tmp_path / "work"
    outside = tmp_path / "work-escape"
    workspace.mkdir()
    outside.mkdir()
    outside_file = outside / "credential.txt"
    outside_file.write_text("outside", encoding="utf-8")
    monkeypatch.chdir(workspace)

    calls = (
        (read_file_tool, {"file_path": str(outside_file)}),
        (write_file_tool, {"file_path": str(outside_file), "content": "changed"}),
        (edit_file_tool, {
            "file_path": str(outside_file),
            "old_str": "outside",
            "new_str": "changed",
        }),
    )
    for module, arguments in calls:
        result = json.loads(module.execute(arguments, sandbox_endpoint=None))
        assert "Access denied" in result["error"]

    link = workspace / "linked-outside"
    link.symlink_to(outside, target_is_directory=True)
    result = json.loads(read_file_tool.execute(
        {"file_path": str(link / outside_file.name)}, sandbox_endpoint=None,
    ))
    assert "Access denied" in result["error"]
    assert outside_file.read_text(encoding="utf-8") == "outside"


def test_web_fetch_requires_public_dns_and_rejects_url_credentials(monkeypatch):
    def fake_getaddrinfo(hostname, _port, **_kwargs):
        address = "93.184.216.34" if hostname == "public.example" else "127.0.0.1"
        return [(2, 1, 6, "", (address, 443))]

    monkeypatch.setattr(web_fetch_tool.socket, "getaddrinfo", fake_getaddrinfo)

    assert web_fetch_tool._is_blocked("https://public.example/page") is None
    assert web_fetch_tool._is_blocked("https://private.example/page") is not None
    assert web_fetch_tool._is_blocked("http://2130706433/latest") is not None
    assert web_fetch_tool._is_blocked(
        "https://name" + chr(64) + "public.example/page"
    ) is not None


class _McpSession:
    def __init__(self, result):
        self._result = result
        self.calls = []

    async def call_tool(self, name, arguments):
        self.calls.append((name, arguments))
        return self._result


def test_mcp_blocks_secret_arguments_and_redacts_server_result():
    secret = "mcp-secret-value-998877665544"
    register_secret("provider.mcp", secret)
    result = SimpleNamespace(
        content=[SimpleNamespace(text=f"server echoed {secret}")],
        isError=False,
    )
    bridge = _AsyncMCPBridge(MCPServerConfig(name="test"))
    bridge._runnable = True
    bridge._session = _McpSession(result)

    blocked = asyncio.run(bridge.call_tool("unsafe", {"value": secret}))
    assert blocked["status"] == "security_policy_blocked"
    assert bridge._session.calls == []

    response = asyncio.run(bridge.call_tool("safe", {"value": "ok"}))
    assert bridge._session.calls == [("safe", {"value": "ok"})]
    assert secret not in json.dumps(response)
    assert "<redacted:provider.mcp>" in response["result"]


def test_trusted_media_rejects_unsafe_or_credential_bearing_urls():
    value = "registered-media-" + "value-998877665544"
    register_secret("provider.media", value)

    with pytest.raises(TrustedMediaError, match="HTTPS"):
        validate_credential_url("http://example.invalid/v1")
    with pytest.raises(TrustedMediaError, match="userinfo"):
        validate_credential_url("https://name@example.invalid/v1")
    with pytest.raises(TrustedMediaError, match="userinfo"):
        with materialize_media("https://name" + chr(64) + "example.invalid/image.png"):

            pass
    with pytest.raises(TrustedMediaError, match="credential material"):
        validate_credential_url(f"https://example.invalid/v1?value={value}")
    with pytest.raises(TrustedMediaError, match="credential material"):
        with materialize_media(f"https://example.invalid/{value}.png"):
            pass


def test_trusted_media_rejects_workspace_escape_and_protected_paths(
    monkeypatch, tmp_path
):
    monkeypatch.setenv("AF_AGENT_PACKAGE_DIR", str(tmp_path))

    for unsafe_path in ("../escape.png", "harness/skills/private.png"):
        with pytest.raises(TrustedMediaError):
            with materialize_media(unsafe_path):
                pass


def test_publish_media_blocks_registered_secret_before_writing(
    monkeypatch, tmp_path
):
    value = "registered-output-" + "value-998877665544"
    register_secret("provider.output", value)
    monkeypatch.setenv("AF_AGENT_PACKAGE_DIR", str(tmp_path))
    source = tmp_path / "source.png"
    source.write_bytes(value.encode())
    destination = tmp_path / "outputs" / "result.png"

    with pytest.raises(TrustedMediaError, match="credential isolation"):
        publish_media(str(source), "outputs/result.png")

    assert not destination.exists()


def test_publish_media_returns_isolated_host_workspace_path(
    monkeypatch, tmp_path
):
    monkeypatch.setenv("AF_AGENT_PACKAGE_DIR", str(tmp_path))
    monkeypatch.setenv("AF_USER_ID", "user-a")
    monkeypatch.setenv("AF_CLIENT_ID", "conversation-a")
    source = tmp_path / "source.png"
    source.write_bytes(b"safe generated image")

    published = publish_media(str(source), "outputs/result.png")

    assert published.startswith("workspace/")
    assert published.endswith("/outputs/result.png")
    assert (tmp_path / published).read_bytes() == b"safe generated image"

    source.unlink()
    published_again = publish_media(str(tmp_path / published), published)

    assert published_again == published
    assert (tmp_path / published).read_bytes() == b"safe generated image"
