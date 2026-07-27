"""Shared sandbox patch utilities — SSL bypass + path-based envd URL routing.

Extracted from the duplicate patch logic in:
- ``examples/agent/outfit-assistant/harness/tools/bash.py`` (L85-92, L33-49)
- ``docker/sandbox/e2b_lifecycle.py`` (_apply_ssl_patch, L80-94)
- ``docker/sandbox/e2b_sync.py`` (_patched_ssl_ctx L44-52, _patch_envd_url L82-95)

These patches are required because the internal E2B deployment uses a
path-based reverse proxy (cubeproxy) with ``server_name _;`` which does
not accept ``*.cube.app`` TLS SNI.  All SDK HTTP calls must be routed
through ``https://218.245.96.148:443/sandbox/{id}/{port}`` with SSL
certificate verification disabled.
"""

# ---------------------------------------------------------------------------
# Deployment constants
# ---------------------------------------------------------------------------

DEFAULT_ENVD_PORT = 49983
SANDBOX_TARGET_HOST = "https://218.245.96.148:443"


# ---------------------------------------------------------------------------
# SSL patch
# ---------------------------------------------------------------------------

def apply_ssl_patch() -> None:
    """Globally patch ``ssl.create_default_context`` to skip certificate verification.

    Idempotent — the monkey-patch is applied only once even if called
    multiple times (guarded by ``ssl._agentflow_patched`` sentinel).

    Source: e2b_lifecycle.py ``_apply_ssl_patch()`` + outfit bash.py L85-92.
    """
    import ssl

    if getattr(ssl, "_agentflow_patched", False):
        return  # already patched — idempotent guard

    _orig = ssl.create_default_context

    def _patched(*args, **kwargs):
        ctx = _orig(*args, **kwargs)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        return ctx

    ssl.create_default_context = _patched
    ssl._agentflow_patched = True  # type: ignore[attr-defined]


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
