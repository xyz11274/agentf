#!/usr/bin/env python3
"""Context updater node (runs before model/skill access).

Fills the customizable prompt template (prompts/context_prompt.md) with config
variables + runtime values and emits ``session_context`` / ``filled_prompt`` as
declared outputs (published by the framework to the central DataBus).

Realizes requirement #2: context assembly before model access, with the filling
logic fully customizable via config/context_config.json + prompts/*.md.
"""
import sys
from pathlib import Path

NODE_DIR = Path(__file__).resolve().parent
if str(NODE_DIR) not in sys.path:
    sys.path.insert(0, str(NODE_DIR))

import harness_bus as hb  # noqa: E402
import harness_lib as hl  # noqa: E402


def main():
    cfg = hl._load_json_file(hl.CONFIG_DIR / "context_config.json", {})
    variables = dict(cfg.get("variables", {})) if isinstance(cfg, dict) else {}
    template_name = (cfg.get("prompt_template") if isinstance(cfg, dict) else "") \
        or "context_prompt.md"
    template = hl.load_prompt_template(template_name)

    text = hb.user_text()
    variables.update({"user_text": text, "history_summary": ""})
    filled = hl.fill_prompt(template, variables)

    session_context = hl.llm_complete(
        "Summarize the user request context in one line:\n" + filled,
        fallback="agent_role=%s; request=%s" % (variables.get("agent_role", ""), text))

    hb.emit_json({
        "session_context": session_context,
        "filled_prompt": filled,
    })


if __name__ == "__main__":
    main()
