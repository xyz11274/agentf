"""Agentflow → Bridge session status sync (Phase 2).

The daemon owns ephemeral execution-slot sessions (pool recyclable). The
Bridge owns durable user sessions. `AgentflowSessionSync` periodically
pulls the daemon's `session.list` for every loaded agent and mirrors
status changes into the Bridge `SessionProvider` so the frontend can
surface "active / completed / destroyed" without polling the daemon.

Design ref: ``docs/design/features/bridge-session-persistence.md`` §5.2.

The sync is best-effort (I-5): any daemon/provider error is logged and
swallowed so a sync outage never breaks the chat main flow or the
background poll loop.
"""

from __future__ import annotations

import logging
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from client import DaemonClient
    from providers import SessionProvider

logger = logging.getLogger("agentflow.bridge.services.session_sync")


class AgentflowSessionSync:
    """Mirror agentflow daemon session status into the Bridge provider.

    Constructed once per process and driven by a background poll loop
    (see ``main.lifespan``). Each ``sync()`` call:
        1. enumerates loaded agents via ``agent.list``
        2. for each agent, fetches ``session.list``
        3. for each daemon session, finds the matching Bridge session
           by ``agentflow_session_id`` and updates its status
    """

    def __init__(
        self,
        daemon_client: DaemonClient,
        provider: SessionProvider,
    ) -> None:
        self._daemon = daemon_client
        self._provider = provider

    async def sync(self) -> int:
        """Run one sync pass. Returns the number of Bridge sessions updated.

        Always returns (never raises) — failures are logged at WARNING.
        """
        updated = 0
        try:
            agents = await self._daemon.agent_list()
        except Exception as exc:  # noqa: BLE101
            logger.warning("session_sync.agent_list failed: %s", exc)
            return 0

        if not agents:
            return 0

        for agent in agents:
            agent_id = agent.get("id") or agent.get("agent_id") or ""
            if not agent_id:
                continue
            updated += await self._sync_agent(agent_id)

        if updated:
            logger.info("session_sync.done updated=%d", updated)
        return updated

    async def _sync_agent(self, agent_id: str) -> int:
        """Sync sessions for one agent. Swallows all errors."""
        try:
            result = await self._daemon.session_list(agent_id)
        except Exception as exc:  # noqa: BLE101
            logger.debug("session_sync.list agent=%s failed: %s", agent_id, exc)
            return 0

        af_sessions = result if isinstance(result, list) else (
            result.get("sessions", []) if isinstance(result, dict) else []
        )

        updated = 0
        for af_sess in af_sessions:
            if not isinstance(af_sess, dict):
                continue
            af_sid = af_sess.get("id") or af_sess.get("session_id") or ""
            status = af_sess.get("status") or ""
            if not af_sid or not status:
                continue
            try:
                bsession = await self._provider.find_by_agentflow_session(af_sid)
                if bsession and bsession.get("status") != status:
                    await self._provider.update_session_status(bsession["id"], status)
                    updated += 1
            except Exception as exc:  # noqa: BLE101
                logger.debug(
                    "session_sync.update af=%s status=%s failed: %s",
                    af_sid, status, exc,
                )
        return updated


__all__ = ["AgentflowSessionSync"]
