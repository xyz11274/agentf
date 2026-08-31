"""Path-based E2B envd routing with endpoint-scoped TLS verification.

The internal cubeproxy is reached by a fixed IP while its certificate names
the cube domain.  Only envd transports therefore disable hostname matching;
the certificate chain remains mandatory.  The process-wide SSL factory and
all credential-bearing E2B control-plane requests remain untouched.
"""

# ---------------------------------------------------------------------------
# Deployment constants
# ---------------------------------------------------------------------------

DEFAULT_ENVD_PORT = 49983
SANDBOX_TARGET_HOST = "https://218.245.96.148:443"


# ---------------------------------------------------------------------------
# Endpoint-scoped envd TLS patch
# ---------------------------------------------------------------------------

_ENVD_TLS_PATCHED = False


def create_envd_ssl_context():
    """Return a chain-verifying context for the fixed-IP envd proxy."""
    import os
    import ssl

    ca_file = os.environ.get("SSL_CERT_FILE", "").strip()
    context = ssl.create_default_context(cafile=ca_file or None)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_REQUIRED
    return context


def apply_ssl_patch() -> None:
    """Patch only E2B envd transports; never modify global SSL behavior."""
    global _ENVD_TLS_PATCHED

    import asyncio
    import importlib
    import os

    from agentflow_runtime.security import register_environment_secrets
    from agentflow_runtime.security.trusted_media import validate_credential_url

    register_environment_secrets()
    api_url = os.environ.get("E2B_API_URL", "").strip()
    if api_url:
        validate_credential_url(api_url)
    if _ENVD_TLS_PATCHED:
        return

    from e2b.api import connection_retries, limits
    from e2b.api.client_sync import EnvdTransportWithLogger

    def _sync_envd_transport(config, http2=True):
        instances = getattr(EnvdTransportWithLogger._thread_local, "instances", {})
        key = (http2, config.proxy, os.environ.get("SSL_CERT_FILE", ""))
        transport = instances.get(key)
        if transport is None:
            transport = EnvdTransportWithLogger(
                limits=limits,
                proxy=config.proxy,
                http2=http2,
                retries=connection_retries,
                verify=create_envd_ssl_context(),
            )
            instances[key] = transport
            EnvdTransportWithLogger._thread_local.instances = instances
        return transport

    sync_modules = (
        "e2b.api.client_sync",
        "e2b.sandbox_sync.commands.command",
        "e2b.sandbox_sync.commands.pty",
        "e2b.sandbox_sync.filesystem.filesystem",
    )
    for module_name in sync_modules:
        module = importlib.import_module(module_name)
        module.get_envd_transport = _sync_envd_transport

    try:
        from e2b.api.client_async import AsyncEnvdTransportWithLogger

        def _async_envd_transport(config, http2=True):
            loop = asyncio.get_running_loop()
            instances = AsyncEnvdTransportWithLogger._instances.get(loop)
            if instances is None:
                instances = {}
                AsyncEnvdTransportWithLogger._instances[loop] = instances
            key = (http2, config.proxy, os.environ.get("SSL_CERT_FILE", ""))
            transport = instances.get(key)
            if transport is None:
                transport = AsyncEnvdTransportWithLogger(
                    limits=limits,
                    proxy=config.proxy,
                    http2=http2,
                    retries=connection_retries,
                    verify=create_envd_ssl_context(),
                )
                instances[key] = transport
            return transport

        for module_name in (
            "e2b.api.client_async",
            "e2b.sandbox_async.main",
        ):
            module = importlib.import_module(module_name)
            module.get_envd_transport = _async_envd_transport
    except ImportError:
        pass

    _ENVD_TLS_PATCHED = True


# ---------------------------------------------------------------------------
# envd URL patch
# ---------------------------------------------------------------------------

def patch_envd_url(sb) -> None:
    """Rewrite the sandbox's ``envd_api_url`` from hostname routing to path-based routing.

    SDK default: ``https://{port}-{id}.cube.app``
    Patched:     ``https://218.245.96.148:443/sandbox/{id}/{port}``

    Patches all known URL attributes on the Sandbox instance and its
    internal sub-clients (filesystem, commands, pty).

    Source: e2b_sync.py ``_patch_envd_url()`` + outfit bash.py ``_patch_sandbox_envd_url()``.
    """
    path = f"{SANDBOX_TARGET_HOST}/sandbox/{sb.sandbox_id}/{DEFAULT_ENVD_PORT}"

    # Private name-mangled attribute (SandboxBase.__envd_api_url)
    try:
        sb._SandboxBase__envd_api_url = path
    except AttributeError:
        pass

    # Public-ish internal attribute
    sb._envd_api_url = path

    # Sub-client modules that hold their own copy of the URL
    for attr in ("_filesystem", "_commands", "_pty"):
        mod = getattr(sb, attr, None)
        if mod is not None and hasattr(mod, "_envd_api_url"):
            mod._envd_api_url = path
