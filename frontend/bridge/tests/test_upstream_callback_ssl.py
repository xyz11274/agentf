"""测试：上游 SysAgent 回调的 HTTPS / mTLS 支持。

覆盖场景：
  - _build_upstream_ssl_context 的各配置组合（单元测试）
  - http_post_to_upstream 的 HTTP/HTTPS 路径（mock httpx）
  - http_post_to_upstream 的网络错误处理
"""

from __future__ import annotations

import ssl
from unittest.mock import AsyncMock, patch

import pytest


# ── Helpers ────────────────────────────────────────────────────────


class _FakeSettings:
    """模拟 BridgeSettings，只暴露 SSL 相关字段。"""

    def __init__(self, **kwargs):
        defaults = dict(
            aisf_adapter_upstream_scheme="http",
            aisf_adapter_upstream_callback_port=23767,
            aisf_adapter_upstream_callback_path="/coremind/systemagent/v1/callback",
            aisf_adapter_upstream_client_cert_path="",
            aisf_adapter_upstream_client_key_path="",
            aisf_adapter_upstream_client_key_password="",
            aisf_adapter_upstream_ca_cert_path="",
            aisf_adapter_upstream_verify=True,
        )
        defaults.update(kwargs)
        for k, v in defaults.items():
            setattr(self, k, v)


def _reload_module():
    """每次测试前重新导入 server 模块，清空模块级缓存。"""
    import sys

    for key in list(sys.modules.keys()):
        if "sysagent.server" in key:
            del sys.modules[key]
    from aisf_adapter.sysagent import server

    return server


# ═══════════════════════════════════════════════════════════════════
#  单元测试：_build_upstream_ssl_context
# ═══════════════════════════════════════════════════════════════════


class TestBuildUpstreamSslContext:
    """_build_upstream_ssl_context 的纯逻辑测试（不涉及网络）。"""

    def test_http_returns_true(self):
        """scheme=http → 返回 True（httpx 默认，不做 SSL）"""
        server = _reload_module()
        settings = _FakeSettings(aisf_adapter_upstream_scheme="http")
        result = server._build_upstream_ssl_context(settings)
        assert result is True

    def test_https_no_customization_returns_true(self):
        """scheme=https 但无证书/无 CA/不跳过验证 → 返回 True（系统默认 CA）"""
        server = _reload_module()
        settings = _FakeSettings(aisf_adapter_upstream_scheme="https")
        result = server._build_upstream_ssl_context(settings)
        assert result is True

    def test_https_skip_verify_returns_ssl_context(self):
        """scheme=https + verify=false → 返回 SSLContext，verify_mode=CERT_NONE"""
        server = _reload_module()
        settings = _FakeSettings(
            aisf_adapter_upstream_scheme="https",
            aisf_adapter_upstream_verify=False,
        )
        result = server._build_upstream_ssl_context(settings)
        assert isinstance(result, ssl.SSLContext)
        assert result.verify_mode == ssl.CERT_NONE
        assert result.check_hostname is False

    def test_https_custom_ca_returns_ssl_context(self):
        """scheme=https + ca_cert_path → 返回 SSLContext"""
        server = _reload_module()
        settings = _FakeSettings(
            aisf_adapter_upstream_scheme="https",
            aisf_adapter_upstream_ca_cert_path="/etc/certs/ca.pem",
        )
        # load_verify_locations 文件必须存在，这里用 monkey-patch 替换
        import ssl as ssl_mod

        orig = ssl_mod.SSLContext.load_verify_locations
        called_with = []

        def fake_load_verify_locations(self_, cafile):
            called_with.append(cafile)

        ssl_mod.SSLContext.load_verify_locations = fake_load_verify_locations
        try:
            result = server._build_upstream_ssl_context(settings)
            assert isinstance(result, ssl.SSLContext)
            assert called_with == ["/etc/certs/ca.pem"]
        finally:
            ssl_mod.SSLContext.load_verify_locations = orig

    def test_https_client_cert_no_password(self):
        """scheme=https + client_cert_path → load_cert_chain 被调用"""
        server = _reload_module()
        settings = _FakeSettings(
            aisf_adapter_upstream_scheme="https",
            aisf_adapter_upstream_client_cert_path="/etc/certs/client.pem",
            aisf_adapter_upstream_client_key_path="/etc/certs/client-key.pem",
        )
        called = []

        import ssl as ssl_mod

        orig = ssl_mod.SSLContext.load_cert_chain
        orig_load_verify = ssl_mod.SSLContext.load_verify_locations

        def fake_load_cert_chain(self_, certfile, keyfile=None, password=None):
            called.append((certfile, keyfile, password))

        def fake_load_verify(self_, *a, **kw):
            pass  # 忽略 CA 调用

        ssl_mod.SSLContext.load_cert_chain = fake_load_cert_chain
        ssl_mod.SSLContext.load_verify_locations = fake_load_verify
        try:
            result = server._build_upstream_ssl_context(settings)
            assert isinstance(result, ssl.SSLContext)
            assert len(called) == 1
            certfile, keyfile, password = called[0]
            assert certfile == "/etc/certs/client.pem"
            assert keyfile == "/etc/certs/client-key.pem"
            assert password is None  # 无密码
        finally:
            ssl_mod.SSLContext.load_cert_chain = orig
            ssl_mod.SSLContext.load_verify_locations = orig_load_verify

    def test_https_client_cert_with_password(self):
        """scheme=https + client_cert_path + password → load_cert_chain(password=...)"""
        server = _reload_module()
        settings = _FakeSettings(
            aisf_adapter_upstream_scheme="https",
            aisf_adapter_upstream_client_cert_path="/etc/certs/client.pem",
            aisf_adapter_upstream_client_key_path="/etc/certs/client-key.pem",
            aisf_adapter_upstream_client_key_password="secret123",
        )
        called = []

        import ssl as ssl_mod

        orig = ssl_mod.SSLContext.load_cert_chain
        orig_load_verify = ssl_mod.SSLContext.load_verify_locations

        def fake_load_cert_chain(self_, certfile, keyfile=None, password=None):
            called.append((certfile, keyfile, password))

        def fake_load_verify(self_, *a, **kw):
            pass

        ssl_mod.SSLContext.load_cert_chain = fake_load_cert_chain
        ssl_mod.SSLContext.load_verify_locations = fake_load_verify
        try:
            result = server._build_upstream_ssl_context(settings)
            assert isinstance(result, ssl.SSLContext)
            assert len(called) == 1
            certfile, keyfile, password = called[0]
            assert certfile == "/etc/certs/client.pem"
            assert keyfile == "/etc/certs/client-key.pem"
            assert password == "secret123"
        finally:
            ssl_mod.SSLContext.load_cert_chain = orig
            ssl_mod.SSLContext.load_verify_locations = orig_load_verify

    def test_https_client_cert_only_path_no_key(self):
        """只有 client_cert_path 没有 client_key_path → keyfile=None 传给 load_cert_chain"""
        server = _reload_module()
        settings = _FakeSettings(
            aisf_adapter_upstream_scheme="https",
            aisf_adapter_upstream_client_cert_path="/etc/certs/client-only.pem",
        )
        called = []

        import ssl as ssl_mod

        orig = ssl_mod.SSLContext.load_cert_chain
        orig_load_verify = ssl_mod.SSLContext.load_verify_locations

        def fake_load_cert_chain(self_, certfile, keyfile=None, password=None):
            called.append((certfile, keyfile, password))

        def fake_load_verify(self_, *a, **kw):
            pass

        ssl_mod.SSLContext.load_cert_chain = fake_load_cert_chain
        ssl_mod.SSLContext.load_verify_locations = fake_load_verify
        try:
            result = server._build_upstream_ssl_context(settings)
            assert isinstance(result, ssl.SSLContext)
            assert len(called) == 1
            certfile, keyfile, _ = called[0]
            assert certfile == "/etc/certs/client-only.pem"
            assert keyfile is None
        finally:
            ssl_mod.SSLContext.load_cert_chain = orig
            ssl_mod.SSLContext.load_verify_locations = orig_load_verify


# ═══════════════════════════════════════════════════════════════════
#  集成测试：http_post_to_upstream
# ═══════════════════════════════════════════════════════════════════


class TestHttpPostToUpstream:
    """http_post_to_upstream 集成测试（mock httpx.AsyncClient）。"""

    # 所有测试共享一次导入，patch 要打在 server.httpx 上（模块级引用）
    _SERVER = None

    @classmethod
    def _import_server(cls):
        """惰性导入 server 模块一次。"""
        if cls._SERVER is None:
            from aisf_adapter.sysagent import server as srv

            TestHttpPostToUpstream._SERVER = srv
        return cls._SERVER

    async def test_http_success(self):
        """scheme=http → URL 使用 http:// 前缀，POST 成功返回 True"""
        server = self._import_server()

        with patch.object(server.httpx, "AsyncClient") as mock_client_cls:
            mock_instance = AsyncMock()
            mock_instance.post.return_value.raise_for_status.return_value = None
            mock_instance.__aenter__.return_value = mock_instance  # async with → self
            mock_client_cls.return_value = mock_instance

            ok = await server.http_post_to_upstream(
                sys_agent_entity="10.0.0.5",
                body={"result": "ok"},
            )
        assert ok is True

        # 验证 httpx.AsyncClient(verify=...) 被创建
        _, call_kwargs = mock_client_cls.call_args
        assert call_kwargs["verify"] is True  # HTTP 默认 verify=True

        # 验证 POST URL
        mock_instance.post.assert_called_once()
        post_args, post_kw = mock_instance.post.call_args
        url = post_args[0] if post_args else post_kw.get("url", "")
        assert url.startswith("http://10.0.0.5:23767")
        assert "/coremind/systemagent/v1/callback" in url

    async def test_https_passes_ssl_context(self):
        """scheme=https → SSLContext 传给 httpx.AsyncClient(verify=...)"""
        server = self._import_server()
        mock_ctx = ssl.create_default_context()

        # http_post_to_upstream 内部 from config import settings (local import)
        # 通过 patch config.settings 来模拟
        with (
            patch("config.settings") as mock_settings,
            patch.object(server, "_build_upstream_ssl_context", return_value=mock_ctx),
            patch.object(server.httpx, "AsyncClient") as mock_client_cls,
        ):
            mock_settings.aisf_adapter_upstream_scheme = "https"
            mock_settings.aisf_adapter_upstream_callback_port = 23767
            mock_settings.aisf_adapter_upstream_callback_path = (
                "/coremind/systemagent/v1/callback"
            )
            mock_settings.aisf_adapter_upstream_client_cert_path = ""
            mock_settings.aisf_adapter_upstream_client_key_path = ""
            mock_settings.aisf_adapter_upstream_client_key_password = ""
            mock_settings.aisf_adapter_upstream_ca_cert_path = ""
            mock_settings.aisf_adapter_upstream_verify = True

            mock_instance = AsyncMock()
            mock_instance.post.return_value.raise_for_status.return_value = None
            mock_instance.__aenter__.return_value = mock_instance
            mock_client_cls.return_value = mock_instance

            ok = await server.http_post_to_upstream(
                sys_agent_entity="sysagent.example.com",
                body={"result": "ok"},
            )
        assert ok is True
        _, call_kwargs = mock_client_cls.call_args
        assert call_kwargs["verify"] is mock_ctx

        # 验证 URL scheme
        post_args, _ = mock_instance.post.call_args
        assert post_args[0].startswith("https://sysagent.example.com")

    async def test_http_post_failure(self):
        """POST 失败（网络异常）→ 返回 False，不抛异常"""
        server = self._import_server()

        with patch.object(server.httpx, "AsyncClient") as mock_client_cls:
            mock_instance = AsyncMock()
            mock_instance.post.side_effect = RuntimeError("connection refused")
            mock_instance.__aenter__.return_value = mock_instance
            mock_client_cls.return_value = mock_instance

            ok = await server.http_post_to_upstream(
                sys_agent_entity="10.0.0.5",
                body={"result": "fail"},
            )
        assert ok is False

    async def test_http_status_error(self):
        """POST 返回非 2xx → 返回 False"""
        import httpx
        from unittest.mock import Mock

        server = self._import_server()
        with patch.object(server.httpx, "AsyncClient") as mock_client_cls:
            mock_instance = AsyncMock()
            # response 用 Mock 而非 AsyncMock，因为 raise_for_status 是同步调用
            mock_response = Mock()
            mock_response.raise_for_status.side_effect = httpx.HTTPStatusError(
                "500 Server Error",
                request=Mock(),
                response=Mock(),
            )
            mock_instance.post.return_value = mock_response
            mock_instance.__aenter__.return_value = mock_instance
            mock_client_cls.return_value = mock_instance

            ok = await server.http_post_to_upstream(
                sys_agent_entity="10.0.0.5",
                body={"result": "fail"},
            )
        assert ok is False
