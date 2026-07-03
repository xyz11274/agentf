"""Diagnostic client for the CoreMind Bridge C++ SideCar.

Provides service discovery by talking to the C++ coremind_bridge over HTTP.
This client is not used by the Bridge business request path; runtime service
snapshots are pushed in the trusted SideCar-to-Bridge envelope.
"""

from __future__ import annotations

import json
import logging
import os
from typing import Any

import httpx
import yaml

logger = logging.getLogger("coremind.client")


class CoreMindClient:
    """Client for the C++ coremind_bridge sidecar.

    Resolves service instances via CoreMind service discovery.
    Uses the C++ sidecar's HTTP API which internally calls
    CMFrm::OM::MicroserviceManager.

    Usage:
        client = CoreMindClient()
        ip, port = client.resolve("IMInferScheduler")
        # → ("10.0.0.5", 27321)
        url = client.resolve_url("IMInferScheduler", "/infermind/llm/v1")
        # → "https://10.0.0.5:27321/infermind/llm/v1"
    """

    def __init__(
        self,
        bridge_url: str = "",
        socket_path: str = "",
        fallback_config: str = "",
    ):
        self._bridge_url = bridge_url or os.environ.get(
            "COREMIND_BRIDGE_URL", "http://127.0.0.1:23767"
        )
        self._socket_path = socket_path or os.environ.get(
            "COREMIND_BRIDGE_SOCKET", ""
        )
        self._fallback_path = fallback_config or os.environ.get(
            "COREMIND_FALLBACK_CONFIG", ""
        )
        self._http = httpx.AsyncClient(timeout=5.0)
        self._cache: dict[str, tuple[str, int]] = {}
        self._available: bool | None = None  # None = not checked yet

    async def check_health(self) -> bool:
        """Check if the C++ bridge is reachable."""
        try:
            resp = await self._http.get(f"{self._bridge_url}/health")
            return resp.status_code == 200
        except (httpx.ConnectError, httpx.TimeoutError):
            return False

    async def is_available(self) -> bool:
        """Lazily check availability on first call."""
        if self._available is None:
            self._available = await self.check_health()
            if self._available:
                logger.info("CoreMind bridge available at %s", self._bridge_url)
            else:
                logger.warning(
                    "CoreMind bridge unavailable at %s, "
                    "will use fallback config",
                    self._bridge_url,
                )
        return self._available

    async def resolve(self, service_name: str) -> tuple[str, int]:
        """Resolve a service name to (ip, port) via CoreMind.

        Returns the first healthy instance. Raises RuntimeError on failure.
        """
        # ① Check local cache
        if service_name in self._cache:
            return self._cache[service_name]

        # ② Try CoreMind bridge
        if await self.is_available():
            try:
                resp = await self._http.get(
                    f"{self._bridge_url}/resolve/{service_name}"
                )
                if resp.status_code == 200:
                    data = resp.json()
                    instances = data.get("instances", [])
                    if instances:
                        ip = instances[0]["ip"]
                        port = instances[0]["port"]
                        self._cache[service_name] = (ip, port)
                        logger.info("resolved %s → %s:%d", service_name, ip, port)
                        return ip, port
                err = data.get("error", "unknown")
                logger.warning("resolve %s failed: %s", service_name, err)
            except Exception as e:
                logger.warning("resolve %s exception: %s", service_name, e)

        # ③ Fallback: static config
        return await self._resolve_fallback(service_name)

    async def resolve_url(
        self, service_name: str, path: str = "", scheme: str = "https"
    ) -> str:
        """Resolve a service and build a full URL.

        Example:
            resolve_url("IMInferScheduler", "/infermind/llm/v1")
            → "https://10.0.0.5:27321/infermind/llm/v1"
        """
        ip, port = await self.resolve(service_name)
        base = f"{scheme}://{ip}:{port}"
        return base + path if path else base

    async def resolve_batch(
        self, service_names: list[str]
    ) -> dict[str, tuple[str, int]]:
        """Resolve multiple services at once."""
        if await self.is_available():
            try:
                resp = await self._http.post(
                    f"{self._bridge_url}/resolve/batch",
                    json={"services": service_names},
                )
                if resp.status_code == 200:
                    data = resp.json()
                    result: dict[str, tuple[str, int]] = {}
                    for name, svc in data.get("services", {}).items():
                        instances = svc.get("instances", [])
                        if instances:
                            ip = instances[0]["ip"]
                            port = instances[0]["port"]
                            result[name] = (ip, port)
                            self._cache[name] = (ip, port)
                    return result
            except Exception as e:
                logger.warning("resolve_batch exception: %s", e)

        # Fallback: resolve one by one
        result = {}
        for name in service_names:
            try:
                result[name] = await self.resolve(name)
            except RuntimeError:
                continue
        return result

    async def _resolve_fallback(self, service_name: str) -> tuple[str, int]:
        """Fallback to static YAML config when CoreMind bridge is unavailable."""
        config_path = self._fallback_path or os.environ.get(
            "COREMIND_FALLBACK_CONFIG",
            "config/service_discovery.yaml",
        )
        if not os.path.exists(config_path):
            raise RuntimeError(
                f"CoreMind bridge unavailable and no fallback config at "
                f"{config_path}. Set COREMIND_BRIDGE_URL or "
                f"COREMIND_FALLBACK_CONFIG."
            )
        with open(config_path) as f:
            config = yaml.safe_load(f)
        svc = config.get("services", {}).get(service_name)
        if not svc:
            raise RuntimeError(
                f"service '{service_name}' not found in fallback config"
            )
        url: str = svc["url"]
        # Parse "https://10.0.0.5:27321" into (ip, port)
        # Strip scheme
        host_part = url.split("://")[-1]
        ip_port = host_part.split("/")[0]
        ip, port_str = ip_port.split(":")
        port = int(port_str)
        self._cache[service_name] = (ip, port)
        logger.info("fallback resolved %s → %s:%d", service_name, ip, port)
        return ip, port

    async def close(self):
        await self._http.aclose()
