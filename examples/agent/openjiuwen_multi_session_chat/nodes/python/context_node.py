#!/usr/bin/env python3
import sys
from pathlib import Path

NODE_ROOT = Path(__file__).resolve().parent
if str(NODE_ROOT) not in sys.path:
    sys.path.insert(0, str(NODE_ROOT))

from compat.context_adapter import fill_context, history_tail, read_history_json, read_prompt_template, summarize_history
from compat.runtime_bridge import DataBusBridge, current_session_id, emit_json, env_text


def main() -> None:
    action = sys.argv[1] if len(sys.argv) > 1 else "read_prompt"
    databus = DataBusBridge()
    session_id = current_session_id()
    if action == "read_prompt":
        emit_json({"prompt_template": read_prompt_template()})
        return
    if action == "read_history":
        history_json = env_text("AF_HISTORY_JSON") or read_history_json(session_id, databus)
        emit_json({"history_tail": history_tail(history_json)})
        return
    if action == "summarize":
        prompt_template = env_text("AF_PROMPT_TEMPLATE") or read_prompt_template()
        history_tail_text = env_text("AF_HISTORY_TAIL")
        emit_json(summarize_history(prompt_template, history_tail_text))
        return
    if action == "fill_context":
        history_summary = env_text("AF_HISTORY_SUMMARY")
        session_context = fill_context(session_id, history_summary, databus)
        emit_json({"session_context": session_context})
        return
    emit_json({"error": f"unsupported action: {action}"})


if __name__ == "__main__":
    main()
