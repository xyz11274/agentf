"""
test_mcp_bridge — MCP bridge integration tests.

Covers:
  4a. discover_servers parsing (stdio + streamable-http)
  4b. tool naming format mcp__{server}__{tool}
  4c. real end-to-end stdio link via FastMCP (F13, no mock)
  4d. failure degradation: missing command -> ensure_ready False, no raise
  4e. execute() routing (I-2/I-3/I-6) with fake injected bridge
"""

import json
import sys
import textwrap
from types import SimpleNamespace

import pytest

from agentflow_runtime.harness.mcp import mcp_bridge


# 4a. discover_servers parsing
def test_discover_servers_stdio_and_http(tmp_path):
    cfg_file = tmp_path / ".mcp.json"
    cfg_file.write_text(json.dumps({
        "mcpServers": {
            "local": {
                "type": "stdio",
                "command": "uv",
                "args": ["run", "server.py"],
                "env": {"FOO": "bar"}
            },
            "remote": {
                "type": "streamable-http",
                "url": "http://example.com:8080/mcp",
                "headers": {"Authorization": "Bearer x"}
            }
        }
    }))
    servers = mcp_bridge.discover_servers(str(cfg_file))
    assert len(servers) == 2
    by_name = {s.name: s for s in servers}

    s_local = by_name["local"]
    assert s_local.transport_type == "stdio"
    assert s_local.command == "uv"
    assert s_local.args == ["run", "server.py"]
    assert s_local.env == {"FOO": "bar"}

    s_remote = by_name["remote"]
    assert s_remote.transport_type == "streamable-http"
    assert s_remote.url == "http://example.com:8080/mcp"
    assert s_remote.headers == {"Authorization": "Bearer x"}


def test_discover_servers_missing_file(tmp_path):
    servers = mcp_bridge.discover_servers(str(tmp_path / "nope.json"))
    assert servers == []


# D1: 畸形配置（null/非 dict）只跳过该 server，不拖垮其他 server（F1-边界）
def test_discover_servers_skips_invalid_entry(tmp_path):
    cfg_file = tmp_path / ".mcp.json"
    cfg_file.write_text(json.dumps({
        "mcpServers": {
            "broken": None,                      # 畸形：null 配置
            "not_dict": "just a string",         # 畸形：非 dict
            "good": {
                "type": "stdio",
                "command": "uv",
                "args": ["run", "server.py"],
            }
        }
    }))
    servers = mcp_bridge.discover_servers(str(cfg_file))
    # 畸形条目被跳过，正常条目仍被解析
    names = [s.name for s in servers]
    assert names == ["good"]
    assert servers[0].command == "uv"


def test_remote_http_client_keeps_tls_and_disables_redirects(monkeypatch):
    captured = {}

    class _FakeAsyncClient:
        def __init__(self, **kwargs):
            captured.update(kwargs)

    monkeypatch.setitem(
        sys.modules,
        "httpx",
        SimpleNamespace(AsyncClient=_FakeAsyncClient),
    )

    client = mcp_bridge._unverified_http_client(
        headers={"Authorization": "Bearer opaque"},
        timeout=12,
    )

    assert isinstance(client, _FakeAsyncClient)
    assert captured["verify"] is True
    assert captured["follow_redirects"] is False
    assert captured["headers"] == {"Authorization": "Bearer opaque"}
    assert captured["timeout"] == 12


@pytest.mark.parametrize(
    "url",
    [
        "https://mcp.example.test/tools",
        "http://127.0.0.1:8199/mcp",
        "http://localhost:8199/mcp",
        "http://[::1]:8199/mcp",
    ],
)
def test_validate_remote_transport_accepts_authenticated_or_loopback(url):
    mcp_bridge._validate_remote_transport(
        mcp_bridge.MCPServerConfig(
            name="safe",
            transport_type="streamable-http",
            url=url,
        )
    )


@pytest.mark.parametrize(
    ("url", "message"),
    [
        ("http://mcp.example.test/tools", "must use HTTPS"),
        (
            "https://user:" + "password@mcp.example.test/tools",
            "must be supplied as headers",
        ),
        (
            "https://mcp.example.test/tools?api_key=credential-value-123456",
            "must not contain credentials",
        ),
        ("ftp://mcp.example.test/tools", "must use HTTPS"),
        ("https:///missing-host", "must use HTTPS"),
    ],
)
def test_validate_remote_transport_rejects_unsafe_urls(url, message):
    with pytest.raises(ValueError, match=message):
        mcp_bridge._validate_remote_transport(
            mcp_bridge.MCPServerConfig(
                name="unsafe",
                transport_type="streamable-http",
                url=url,
            )
        )
# 4b. tool naming — requires mcp SDK for a real stdio server.
# Defer to the real-link test (4c) which also validates naming.

# Minimal stdio server script for 4c and 4b
_ECHO_SERVER_SCRIPT = textwrap.dedent('''\
    import sys
    from mcp.server.fastmcp import FastMCP

    mcp = FastMCP("echo-server")

    @mcp.tool()
    def echo(text: str) -> str:
        """Echo back the input text."""
        return f"echo:{text}"

    if __name__ == "__main__":
        mcp.run(transport="stdio")
    ''')


def _write_echo_server(tmp_path):
    server_py = tmp_path / "echo_server.py"
    server_py.write_text(_ECHO_SERVER_SCRIPT)
    return str(server_py)


# 4c + 4b. real end-to-end link
def test_real_stdio_echo_and_naming(tmp_path):
    pytest.importorskip("mcp")
    server_path = _write_echo_server(tmp_path)

    cfg = mcp_bridge.MCPServerConfig(
        name="echo",
        transport_type="stdio",
        command=sys.executable,
        args=[server_path],
    )
    bridge = mcp_bridge.MCPBridge(cfg)
    try:
        assert bridge.ensure_ready() is True
        tools = bridge.load_tools()
        assert len(tools) >= 1
        # 4b: tool name format mcp__{server}__{tool}
        echo_tool = next(
            (t for t in tools if t["function"]["name"] == "mcp__echo__echo"),
            None,
        )
        assert echo_tool is not None, f"expected mcp__echo__echo, got {[t['function']['name'] for t in tools]}"

        # 4c: real call_tool
        result = bridge.call_tool("echo", {"text": "hello world"})
        assert "error" not in result, f"unexpected error: {result}"
        assert "echo:hello world" in result.get("result", "")
    finally:
        bridge.shutdown()


# 4d. failure degradation
def test_missing_command_degrades(tmp_path):
    cfg = mcp_bridge.MCPServerConfig(
        name="ghost",
        transport_type="stdio",
        command="/nonexistent/command/that/does/not/exist",
        args=[],
    )
    bridge = mcp_bridge.MCPBridge(cfg)
    try:
        # ensure_ready returns False, does NOT raise
        ready = bridge.ensure_ready()
        assert ready is False
        # call_tool on unconnected bridge returns error dict, no raise
        result = bridge.call_tool("any_tool", {})
        assert "error" in result
    finally:
        bridge.shutdown()


# 4e. execute() routing via _LocalToolRegistry
def _make_registry_with_fake_mcp(monkeypatch):
    """Construct _LocalToolRegistry without loading builtin tools; inject fake MCP bridge."""
    from agentflow_runtime import skill_executor as se

    # Avoid side-effects from real builtin tool loading
    monkeypatch.setattr(se, "_load_dir", lambda *a, **kw: None)

    # Stub MCP availability + bridge so _load_mcp_tools injects a fake bridge
    class _FakeBridge:
        def ensure_ready(self):
            return True

        def load_tools(self):
            return [{"type": "function", "function": {
                "name": "mcp__fake__tool",
                "description": "fake",
                "parameters": {"type": "object", "properties": {}}}}]

        def call_tool(self, tool_name, args):
            return {"result": f"fake:{tool_name}:{json.dumps(args)}",
                    "isError": False, "tool": tool_name}

    monkeypatch.setattr(se, "MCP_AVAILABLE", True)
    monkeypatch.setattr(se, "MCPBridge", lambda cfg: _FakeBridge())
    monkeypatch.setattr(
        se, "discover_servers",
        lambda *paths: [mcp_bridge.MCPServerConfig(name="fake", transport_type="stdio", command="x")])
    # Suppress env config path interference
    monkeypatch.setenv("AF_MCP_CONFIG_PATHS", "")

    registry = se._LocalToolRegistry()
    return registry


def test_execute_routes_mcp_prefix(monkeypatch):
    registry = _make_registry_with_fake_mcp(monkeypatch)
    # MCP-prefixed name routes to fake bridge
    out = registry.execute("mcp__fake__tool", {"k": "v"})
    parsed = json.loads(out)
    assert "error" not in parsed, f"unexpected error: {parsed}"
    assert "fake:tool" in parsed.get("result", "")


def test_execute_non_mcp_falls_through(monkeypatch):
    """Non-mcp__ prefixed unknown tool returns Unknown tool (I-2 original path)."""
    registry = _make_registry_with_fake_mcp(monkeypatch)
    out = registry.execute("some_unknown_tool", {})
    parsed = json.loads(out)
    assert "error" in parsed
    assert "Unknown tool" in parsed["error"]


def test_execute_mcp_unknown_server(monkeypatch):
    """mcp__ prefix with unknown server returns error JSON, no raise (I-3)."""
    registry = _make_registry_with_fake_mcp(monkeypatch)
    out = registry.execute("mcp__nosuch__tool", {})
    parsed = json.loads(out)
    assert "error" in parsed
    assert "not connected" in parsed["error"]


# 4f. _load_mcp_tools 入口/未发现配置日志规范（M3 可观测性：日志含可定位信息）
def _make_registry_without_mcp_config(monkeypatch):
    """Construct _LocalToolRegistry with MCP available but no servers discovered.

    Stubs builtin tool loading (avoid side-effects) + discover_servers returns [] so
    _load_mcp_tools walks the none_config branch.
    """
    from agentflow_runtime import skill_executor as se

    monkeypatch.setattr(se, "_load_dir", lambda *a, **kw: None)
    monkeypatch.setattr(se, "MCP_AVAILABLE", True)
    monkeypatch.setattr(se, "MCPBridge", lambda cfg: pytest.fail("bridge must not be built when no config"))
    monkeypatch.setattr(se, "discover_servers", lambda *paths: [])
    monkeypatch.setenv("AF_MCP_CONFIG_PATHS", "")
    return se


def test_load_mcp_tools_emits_start_log(monkeypatch, caplog):
    """入口日志 mcp_tools.start 含 scan_paths，标明开始扫配置 + 扫哪些路径（排障区分"没跑到这段"vs"配置为空"）。"""
    import logging as _logging

    se = _make_registry_without_mcp_config(monkeypatch)
    # skill_executor 用命名 logger（logger = logging.getLogger(__name__)），按命名 logger 捕获
    with caplog.at_level(_logging.INFO, logger="agentflow_runtime.skill_executor"):
        se._LocalToolRegistry()
    start_logs = [r for r in caplog.records if r.message.startswith("mcp_tools.start")]
    assert start_logs, "must emit mcp_tools.start entry log at _load_mcp_tools entry"
    # scan_paths 字段含扫的路径列表（pkg + cwd 两路，去重后）
    assert "scan_paths=" in start_logs[0].message, "start log must carry scan_paths field"


def test_load_mcp_tools_none_config_log_carries_paths(monkeypatch, caplog, tmp_path):
    """未发现配置时 mcp_tools.none_config 日志必须含 scan_paths——排障可定位"路径算错"还是"路径对但文件不存在"。"""
    import logging as _logging

    se = _make_registry_without_mcp_config(monkeypatch)
    # cwd 设到 tmp_path，让 cwd/.mcp.json 路径可断言（tmp_path 下没有 .mcp.json）
    monkeypatch.chdir(tmp_path)
    with caplog.at_level(_logging.INFO, logger="agentflow_runtime.skill_executor"):
        se._LocalToolRegistry()
    none_logs = [r for r in caplog.records if r.message.startswith("mcp_tools.none_config")]
    assert none_logs, "must emit mcp_tools.none_config when no servers discovered"
    msg = none_logs[0].message
    assert "scan_paths=" in msg, "none_config log must carry scan_paths field"
    # cwd 路径必须是 tmp_path/.mcp.json（证路径算对但文件不存在 → none 而非 loaded）
    assert str(tmp_path / ".mcp.json") in msg, f"none_config scan_paths must contain cwd path, got: {msg}"
