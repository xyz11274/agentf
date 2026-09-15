#!/usr/bin/env python3
"""
LLM Call — LLM API call entry point.

Responsibilities (after the responsibility split, see
.agents/.coding_gumdam/workspace/.task-runner/refactor-llm-call/):
  * ``call_llm`` — public LLM call entry (env overrides + trace recording)
  * ``resolve_llm_config`` — re-exported from ``common.clients``
  * trace recording lives in ``agentflow_runtime.recorder`` (TraceRecorder)
  * token estimation & message compaction live in
    ``agentflow_runtime.compaction``

HTTP transport: requests with certificate verification, no ambient proxy,
and no credential-bearing redirect.

No mock mode — always calls the real LLM. For offline testing, point
base_url at a local stub server.
"""

import os
import sys

# ── Add nodes/ to sys.path for local imports ──
_nodes_dir = os.path.dirname(os.path.abspath(__file__))
if _nodes_dir not in sys.path:
    sys.path.insert(0, _nodes_dir)

from agentflow_runtime.common.agent_config import _is_placeholder, resolve_vision_config  # noqa: E402
from agentflow_runtime.common.clients import resolve_llm_config  # noqa: E402

# Re-export for backward compatibility (薄聚合, AC2: 公共符号行为不变)
__all__ = ["call_llm", "resolve_llm_config", "get_recorder",
           "resolve_vision_config", "_is_placeholder"]

from agentflow_runtime.recorder import get_recorder  # noqa: E402

try:
    from evolution_configured import points as _evolution
except ImportError:
    _evolution = None


def call_llm(messages: list, model: str = "gpt-4o-mini",
             base_url: str = "", api_key: str = "",
             tools: list | None = None,
             max_completion_tokens: int = 2000,
             timeout: int = 120,
             connect_timeout: int = 5,
             stream: bool = False,
             source: str = "",
             skill: str = "",
             round_i: int = 0,
             extra_body: dict | None = None) -> dict:
    """Call LLM API with full trace recording. Returns OpenAI-compatible dict.

    Two-stage timeout (see recorder.TraceRecorder.trace_call docstring):
      connect_timeout — TCP+TLS handshake, fast-fail when network is down
      timeout         — whole request, generous for slow LLM generation
    Both overrideable via env vars AF_LLM_CONNECT_TIMEOUT / AF_LLM_TIMEOUT.

    ``stream`` is passed from the parsed input payload — the bridge includes
    it in the ``{stream, messages}`` wrapper object.  The callee
    (coordinator_decide / skill_executor) extracts ``stream`` via
    ``parse_stream(AF_INPUT)`` and passes it here.

    ``source`` / ``skill`` / ``round_i`` identify the caller for the
    streaming chunk classification:
      source="coordinator" / "skill_executor" → on_chunk pushes type="llm_chunk"
        (统一增量通道;消费端按 source 区分:coordinator 进对话正文,
         skill_executor 进思考/过程通道。汇总进度文本由各节点单独推 type="progress")
    Empty source → no chunk push (the on_chunk callback is not wired).

    ``extra_body`` — additional LLM request body parameters (temperature,
    top_p, frequency_penalty, etc.). Passed as-is to the HTTP/UDS client.
    Callers should read values from ``AgentConfig`` before calling.
    """
    # Env override (M3: configurable, no hidden config). Callers passing
    # explicit args win; env vars fill the default for callers that don't.
    if "AF_LLM_CONNECT_TIMEOUT" in os.environ:
        try:
            connect_timeout = int(os.environ["AF_LLM_CONNECT_TIMEOUT"])
        except ValueError:
            pass
    if "AF_LLM_TIMEOUT" in os.environ:
        try:
            timeout = int(os.environ["AF_LLM_TIMEOUT"])
        except ValueError:
            pass
    if _evolution is not None and os.environ.get("AF_COORDINATOR_VERSION", "v1") == "v1":
        return _evolution.call({
            "ref": {
                "run": os.environ.get("AF_DIALOGUE_ID", ""),
                **({"parent": os.environ["AF_EXEC_SKILL_KEY"]}
                   if os.environ.get("AF_EXEC_SKILL_KEY") else {}),
            },
            "kind": "model", "name": "model_call", "actor_id": skill or source or "model",
            "input": {"messages": messages, "model": model, "tools": tools,
                      "max_completion_tokens": max_completion_tokens},
            "extensions": {"source": source, "round": round_i},
        }, lambda request: get_recorder().trace_call(
            request["messages"], model, base_url, api_key, tools,
            max_completion_tokens, timeout, connect_timeout, stream,
            source=source, skill=skill, round_i=round_i, extra_body=extra_body),
            user_id=os.environ.get("AF_USER_ID", ""))
    return get_recorder().trace_call(
        messages, model, base_url, api_key, tools,
        max_completion_tokens, timeout, connect_timeout, stream,
        source=source, skill=skill, round_i=round_i,
        extra_body=extra_body)
