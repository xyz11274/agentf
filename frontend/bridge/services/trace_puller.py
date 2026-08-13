"""Bridge-side trace pulling service.

After a dialogue completes (the ``[DONE]`` signal), the Bridge pulls the
LLM execution traces that the harness agent wrote to the DataBus under
``llm/traces/{call_id}/{meta,request,response}`` in the agent's
session-isolated namespace, maps them to the Bridge ``session_id``, and
persists them via the configured :class:`~providers.SessionProvider`.

The agent's ``llm_call.py`` / ``TraceRecorder`` writes traces via the
DataBus Python SDK (``agentflow_databus``), which goes through the
per-session UDS server. The UDS server prepends
``/session/{session_id}/`` to every key, so the full DataBus path is::

    /session/{session_id}/llm/traces/{call_id}/request
    /session/{session_id}/llm/traces/{call_id}/response
    /session/{session_id}/llm/traces/{call_id}/meta

The Bridge queries these traces through the daemon's ``databus.list_keys``
/ ``databus.get`` RPC (ADR-0004). The daemon's RPC handler automatically
prepends ``/session/{session_id}/`` to the prefix/key sent by the Bridge,
so the Bridge sends *relative* keys (without the ``/session/`` prefix).

For example::

    Bridge sends prefix="llm/traces/"
    → daemon queries "/session/{session_id}/llm/traces/"
    → daemon strips the session prefix from returned keys
    → Bridge receives "llm/traces/{call_id}/meta"

Design references:
    * ``docs/design/features/bridge-session-persistence.md`` §5.3
    * ADR-0004: ``docs/design/adrs/0004-llm-trace-data-bus-bridge.md``

The puller uses the daemon's existing ``databus.list_keys`` /
``databus.get`` RPC (no direct UDS). All failures are logged and
swallowed (I-5: trace persistence is non-critical and must not break
the chat main flow).
"""

from __future__ import annotations

import json
import logging
import time
from typing import Any

from client import DaemonClient, DaemonError
from providers import SessionProvider

logger = logging.getLogger("agentflow.bridge.services.trace_puller")


def _fix_newlines(value: Any) -> Any:
    """Recursively replace literal ``\\n`` (backslash + n) with actual
    newline characters in every string value within the given data structure.

    This undoes the double-JSON-encoding that happens when the agent's
    ``llm_call.py`` stores dict values via the UDS server: the UDS's
    ``extract_json_string`` serialises the dict to a JSON string, and when
    the daemon's ``handle_databus_get`` embeds that string directly into the
    JSON-RPC response, escape sequences like ``\\n`` end up as literal
    ``\\n`` (backslash + n) instead of actual newline bytes.
    """
    if isinstance(value, str) and "\\n" in value:
        return value.replace("\\n", "\n")
    if isinstance(value, dict):
        return {k: _fix_newlines(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_fix_newlines(v) for v in value]
    return value


# DataBus prefix under the agent's session namespace.  The agent's
# TraceRecorder writes ``llm/traces/{call_id}/{meta,request,response}``
# which, after the UDS server prepends ``/session/{session_id}/``, becomes
# ``/session/{session_id}/llm/traces/...``.
# The Bridge sends this relative prefix to the daemon's ``databus.list_keys``
# RPC, which re-prepends ``/session/{session_id}/`` internally (the daemon
# handler always adds the session prefix — the Bridge must NOT include it).
_TRACE_PREFIX = "llm/traces/"


async def _pull_and_save_traces(
    daemon_client: DaemonClient,
    provider: SessionProvider,
    *,
    agent_id: str,
    user_id: str,
    client_id: str,
    dialogue_id: str,
    session_id: str,
) -> int:
    """Pull dialogue traces from the daemon and persist them to the provider.

    Resolves the daemon's session via (``user_id``, ``client_id``) instead
    of a raw ``session_id`` string because the daemon's ``agent.execute``
    RPC does **not** return a ``session_id``.  The daemon's
    ``handle_databus_list_keys`` handler accepts ``user_id`` + ``client_id``
    and resolves the session internally via ``resolve_session_id()``.

    Returns the number of trace entries successfully persisted. Always
    returns (never raises) — failures are logged at WARNING level.
    """
    if not dialogue_id:
        logger.info(
            "trace_pull.skip agent=%s dialogue=%s (missing dialogue_id)",
            agent_id, dialogue_id,
        )
        return 0

    pull_start = time.time()
    key_count = 0

    sess_params: dict[str, Any] = {
        "agent_id": agent_id,
        # Pass (user_id, client_id) instead of session_id so the daemon can
        # resolve the internal session_id via resolve_session_id().  This is
        # needed because agent.execute does not return session_id.
        "user_id": user_id,
        "client_id": client_id,
        "session_id": "",  # daemon will resolve from (user_id, client_id)
    }

    logger.info(
        "trace_pull.start agent=%s user=%s client=%s dialogue=%s sess=%s prefix=%s",
        agent_id, user_id, client_id, dialogue_id, session_id, _TRACE_PREFIX,
    )

    persisted = 0
    try:
        list_result = await daemon_client.send_request(
            "databus.list_keys",
            {**sess_params, "prefix": _TRACE_PREFIX},
        )
    except DaemonError as exc:
        logger.info(
            "trace_pull.list_keys_failed agent=%s dialogue=%s user=%s: %s",
            agent_id, dialogue_id, user_id, exc,
        )
        logger.info(
            "trace_pull.done agent=%s dialogue=%s elapsed=%.3fs keys=%d",
            agent_id, dialogue_id, time.time() - pull_start, key_count,
        )
        return 0

    raw_keys: list[str] = list_result.get("keys", []) if isinstance(list_result, dict) else []
    key_count = len(raw_keys)
    logger.info(
        "trace_pull.list_keys_ok agent=%s dialogue=%s user=%s raw_keys_count=%d total=%d raw_keys=%s",
        agent_id, dialogue_id, user_id, len(raw_keys), len(raw_keys),
        raw_keys,
    )

    # Filter to ``/meta`` sub-keys — each one identifies a distinct
    # LLM call (call_id).
    meta_suffix = "/meta"
    call_ids: list[str] = []
    for k in raw_keys:
        if k.endswith(meta_suffix):
            call_id = k[len(_TRACE_PREFIX):-len(meta_suffix)]
            call_ids.append(call_id)

    if not call_ids:
        logger.info(
            "trace_pull.no_traces agent=%s dialogue=%s user=%s "
            "(no /meta keys in %d raw keys)",
            agent_id, dialogue_id, user_id, len(raw_keys),
        )
        logger.info(
            "trace_pull.done agent=%s dialogue=%s elapsed=%.3fs keys=%d",
            agent_id, dialogue_id, time.time() - pull_start, key_count,
        )
        return 0

    logger.info(
        "trace_pull.call_ids agent=%s dialogue=%s call_ids=%s",
        agent_id, dialogue_id, call_ids,
    )

    for call_id in call_ids:
        logger.info(
            "trace_pull.fetch_detail start agent=%s call_id=%s prefix=%s",
            agent_id, call_id, _TRACE_PREFIX,
        )
        trace_data = await _fetch_trace_detail(daemon_client, sess_params, _TRACE_PREFIX, call_id)
        if trace_data is None:
            logger.info(
                "trace_pull.fetch_detail_none agent=%s call_id=%s",
                agent_id, call_id,
            )
            continue
        logger.info(
            "trace_pull.save_start agent=%s call_id=%s session_id=%s dialogue_id=%s",
            agent_id, call_id, session_id, dialogue_id,
        )
        try:
            await provider.save_trace(
                session_id=session_id,
                dialogue_id=dialogue_id,
                trace_type="llm_call",
                trace_data=trace_data,
            )
            persisted += 1
            logger.info(
                "trace_pull.save_ok agent=%s call_id=%s persisted=%d",
                agent_id, call_id, persisted,
            )
            # Clean up this call's DataBus traces immediately after saving.
            # Each call is independent — one failure never blocks the others
            # from being freed, and there is no partial-failure data loss.
            try:
                cleanup_result = await daemon_client.send_request(
                    "databus.remove_by_prefix",
                    {**sess_params, "prefix": f"llm/traces/{call_id}/"},
                )
                removed = cleanup_result.get("removed", 0) if isinstance(cleanup_result, dict) else 0
                logger.info(
                    "trace_pull.cleanup_call agent=%s call_id=%s removed=%d",
                    agent_id, call_id, removed,
                )
            except DaemonError as exc:
                logger.warning(
                    "trace_pull.cleanup_call_failed agent=%s call_id=%s: %s",
                    agent_id, call_id, exc,
                )
        except Exception as exc:  # noqa: BLE001
            # I-5: provider failures must not break the chat flow.
            logger.warning(
                "trace_pull.save_failed agent=%s call_id=%s: %s type=%s",
                agent_id, call_id, exc, type(exc).__name__,
            )

    logger.info(
        "trace_pull.done agent=%s dialogue=%s persisted=%d",
        agent_id, dialogue_id, persisted,
    )

    # After all calls, clean up the dialogue-scoped index key.
    if persisted > 0:
        try:
            index_cleanup = await daemon_client.send_request(
                "databus.remove_by_prefix",
                {**sess_params, "prefix": f"llm/traces/index/{dialogue_id}"},
            )
            removed_index = index_cleanup.get("removed", 0) if isinstance(index_cleanup, dict) else 0
            logger.info(
                "trace_pull.cleanup_index agent=%s dialogue=%s removed=%d",
                agent_id, dialogue_id, removed_index,
            )
        except DaemonError as exc:
            logger.warning(
                "trace_pull.cleanup_index_failed agent=%s dialogue=%s: %s",
                agent_id, dialogue_id, exc,
            )

    logger.info(
        "trace_pull.done agent=%s dialogue=%s elapsed=%.3fs keys=%d",
        agent_id, dialogue_id, time.time() - pull_start, key_count,
    )
    return persisted


async def _fetch_trace_detail(
    daemon_client: DaemonClient,
    sess_params: dict[str, Any],
    prefix: str,
    call_id: str,
) -> dict[str, Any] | None:
    """Fetch ``request``, ``response``, ``meta`` sub-keys for one call_id.

    Returns ``None`` on any daemon error so the caller can skip the entry
    without aborting the whole pull.
    """
    fetch_start = time.time()
    detail: dict[str, Any] = {"call_id": call_id}

    async def _safe_get(key: str) -> Any:
        databus_key = f"{prefix}{call_id}/{key}"
        logger.info(
            "trace_pull.safe_get call_id=%s key=%s databus_key=%s",
            call_id, key, databus_key,
        )
        try:
            result = await daemon_client.send_request(
                "databus.get",
                {**sess_params, "key": databus_key},
            )
        except DaemonError as exc:
            logger.info(
                "trace_pull.get_failed call_id=%s key=%s databus_key=%s: %s",
                call_id, key, databus_key, exc,
            )
            return None
        if not isinstance(result, dict) or not result.get("exists"):
            logger.info(
                "trace_pull.get_not_exists call_id=%s key=%s databus_key=%s result=%s",
                call_id, key, databus_key, result,
            )
            return None
        raw_value = result.get("value")
        # Debug: log type and full raw_value to diagnose \n encoding (F7: 不截断)
        if raw_value is not None:
            raw_str = str(raw_value)
            logger.info(
                "trace_pull.raw_value call_id=%s key=%s value_type=%s len=%d value=%s",
                call_id, key, type(raw_value).__name__, len(raw_str), raw_str,
            )
        else:
            logger.info(
                "trace_pull.raw_value call_id=%s key=%s value_type=None value=None",
                call_id, key,
            )
        # The agent's llm_call.py stores dict values (request/response/meta)
        # through the UDS server which extracts them as JSON strings.
        # The value returned by databus.get is a JSON-encoded string of the
        # original dict. Parse it back to a dict for the frontend.
        if isinstance(raw_value, str):
            try:
                parsed = json.loads(raw_value)
                logger.info(
                    "trace_pull.get_parsed call_id=%s key=%s original_type=%s parsed_type=%s",
                    call_id, key, type(raw_value).__name__, type(parsed).__name__,
                )
                return _fix_newlines(parsed)
            except (json.JSONDecodeError, TypeError):
                # Not a JSON string — return as-is.
                pass
        logger.info(
            "trace_pull.get_ok call_id=%s key=%s databus_key=%s value_type=%s",
            call_id, key, databus_key,
            type(raw_value).__name__,
        )
        return _fix_newlines(raw_value)

    # Use sequential gets — call_id count is small (typically ≤ 10 per
    # dialogue) and concurrency here would only add complexity without
    # measurable latency gain.
    request_data = await _safe_get("request")
    response_data = await _safe_get("response")
    meta_data = await _safe_get("meta")

    if request_data is None and response_data is None and meta_data is None:
        logger.info(
            "trace_pull.fetch_detail_done call_id=%s elapsed=%.3fs result=none",
            call_id, time.time() - fetch_start,
        )
        return None

    detail["request"] = request_data
    detail["response"] = response_data
    detail["meta"] = meta_data
    logger.info(
        "trace_pull.fetch_detail_done call_id=%s elapsed=%.3fs result=ok",
        call_id, time.time() - fetch_start,
    )
    return detail


__all__ = ["_pull_and_save_traces"]
