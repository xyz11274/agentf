"""
frontend/bridge/monitor.py — Daemon 连接状态监控

后台任务每 5s 检查 daemon 连接，更新状态缓存供 /api/daemon/status 使用。
"""

import asyncio
import logging
import time
from typing import Optional

logger = logging.getLogger(__name__)


class DaemonMonitor:
    """Monitors daemon connection health and caches status."""

    def __init__(self, check_health_fn, interval: float = 5.0):
        self._check_health = check_health_fn
        self._interval = interval
        self._task: Optional[asyncio.Task] = None
        self._cached_status: dict = {"running": False, "last_check": 0}
        self._running = False

    @property
    def cached_status(self) -> dict:
        return dict(self._cached_status)

    async def start(self):
        self._running = True
        self._task = asyncio.create_task(self._loop())
        logger.info("DaemonMonitor started (interval=%ss)", self._interval)

    async def stop(self):
        self._running = False
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
            self._task = None
        logger.info("DaemonMonitor stopped")

    async def _loop(self):
        while self._running:
            try:
                status = await self._check_health()
                self._cached_status = {
                    "running": status.get("status") == "ok",
                    "last_check": int(time.time()),
                    **(status if isinstance(status, dict) else {}),
                }
            except Exception as e:
                self._cached_status = {
                    "running": False,
                    "error": str(e),
                    "last_check": int(time.time()),
                }
            await asyncio.sleep(self._interval)
