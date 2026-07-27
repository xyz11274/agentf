"""ToolExecutorFactory — selects the correct provider by env/params.

Reads ``AF_SANDBOX_MODE`` and ``AF_SANDBOX_ENDPOINT`` (or explicit params)
and returns the matching ToolExecutor:

- ``none`` / unset    → HostExecutor   (backward-compatible default)
- ``local`` / ``docker`` → HTTPExecutor (requires non-empty endpoint)
- ``e2b``             → E2BExecutor    (requires non-empty endpoint + E2B_API_KEY)

Explicit parameters take precedence over environment variables.

Reference: ADR 20260724-tools-runtime-refactor.md §"Factory 选择逻辑".
"""

import os

from agentflow_runtime.executors.base import ToolExecutor
from agentflow_runtime.executors.e2b import E2BExecutor
from agentflow_runtime.executors.host import HostExecutor
from agentflow_runtime.executors.http import HTTPExecutor

__all__ = ["create_executor"]


def create_executor(
    sandbox_mode: str | None = None,
    sandbox_endpoint: str | None = None,
) -> ToolExecutor:
    """Return the ToolExecutor matching the requested sandbox mode.

    Args:
        sandbox_mode: Override for ``AF_SANDBOX_MODE``.  Falls back to env,
            then defaults to ``"none"``.
        sandbox_endpoint: Override for ``AF_SANDBOX_ENDPOINT``.  Falls back
            to env, then defaults to empty string.

    Returns:
        HostExecutor / HTTPExecutor / E2BExecutor instance.

    Raises:
        RuntimeError: if ``local``/``docker``/``e2b`` mode is requested but
            the endpoint is empty (signals daemon injection failure).
    """
    mode = sandbox_mode or os.environ.get("AF_SANDBOX_MODE", "none")
    endpoint = sandbox_endpoint or os.environ.get("AF_SANDBOX_ENDPOINT", "")

    if mode == "e2b":
        if not endpoint:
            raise RuntimeError(
                "e2b mode but endpoint empty — daemon injection failed"
            )
        return E2BExecutor(endpoint)
    elif mode in ("local", "docker"):
        if not endpoint:
            raise RuntimeError(f"{mode} mode but endpoint empty")
        return HTTPExecutor(endpoint)
    else:  # "none" or any unrecognized value → host fallback
        return HostExecutor()
