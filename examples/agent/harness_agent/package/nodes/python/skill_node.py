#!/usr/bin/env python3
"""Generic harness skill node (parameterized by skill name via argv[1]).

Each skill: reads accumulated prior results from the shared Store, produces a
deterministic (offline) or LLM-assisted result that *depends on prior results*,
persists its structured envelope, and emits loop-control outputs.
"""
import json
import sys
from pathlib import Path

NODE_DIR = Path(__file__).resolve().parent
if str(NODE_DIR) not in sys.path:
    sys.path.insert(0, str(NODE_DIR))

import harness_bus as hb  # noqa: E402
import harness_lib as hl  # noqa: E402


def _produce(skill, text, results):
    """Deterministic skill behavior; dependent on prior results."""
    if skill == "retrieve":
        body = "围绕「%s」检索到要点：背景、关键约束、相关线索。" % text
        return hl.make_envelope(skill, result=body, confidence=0.8)
    if skill == "analyze":
        prior = results.get("retrieve", {}).get("result", "")
        if not prior:
            return hl.make_envelope(skill, status="unsupported", can_handle=False,
                                    result="", error="no_retrieve_result", confidence=0.0)
        body = "基于检索结果分析得出：核心诉求已识别，给出可执行判断。（依据：%s）" % prior[:40]
        return hl.make_envelope(skill, result=body, confidence=0.85)
    if skill == "compose":
        facts = results.get("retrieve", {}).get("result", "")
        insight = results.get("analyze", {}).get("result", "")
        if not (facts or insight):
            return hl.make_envelope(skill, status="unsupported", can_handle=False,
                                    result="", error="insufficient_inputs", confidence=0.0)
        body = "综合草稿：结合检索与分析，形成初步答复。要点1/2/3。"
        return hl.make_envelope(skill, result=body, confidence=0.9)
    if skill == "finalize":
        draft = results.get("compose", {}).get("result", "")
        final = "最终答复：%s 结论：已完成对「%s」的检索-分析-综合-定稿闭环。" % (draft, text)
        return hl.make_envelope(skill, result=final, confidence=0.95)
    return hl.make_envelope(skill, status="unsupported", can_handle=False,
                            result="", error="unknown_skill", confidence=0.0)


def main():
    skill = sys.argv[1] if len(sys.argv) > 1 else hb.env("AF_SKILL_NAME", "retrieve")
    store = hb.Store()
    env_text = hb.user_text()
    # Persist the original request once; ignore framework trigger artifacts.
    if env_text and env_text not in ("workflow_completed", "workflow_scheduled"):
        store.save_user_text(env_text)
    text = store.load_user_text() or env_text
    prior = store.load_results()

    envelope = _produce(skill, text, prior)

    if envelope.get("status") == "ok":
        envelope["result"] = hl.llm_complete(
            "针对请求「%s」，完善以下%s结果：%s" % (text, skill, envelope["result"]),
            fallback=envelope["result"])

    cfg = hl.load_harness_config()
    envelope = hl.truncate_observation(
        envelope, int(cfg.get("max_observation_length", 600)))
    store.save_result(skill, envelope)
    hb.write_marker("skilldone", skill)

    hb.emit_json({
        "skill_text": envelope.get("result", ""),
        "skill_result": json.dumps(envelope, ensure_ascii=False),
        "status": envelope.get("status", "ok"),
        "can_handle": bool(envelope.get("can_handle", True)),
        "skill_completed": True,
    })


if __name__ == "__main__":
    main()
