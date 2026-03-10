#!/usr/bin/env python3
import json
import sys
from pathlib import Path

NODE_ROOT = Path(__file__).resolve().parent
if str(NODE_ROOT) not in sys.path:
    sys.path.insert(0, str(NODE_ROOT))

from compat.runtime_bridge import DataBusBridge, current_session_id, current_user_text, emit_json, env_text
from compat.skill_adapter import choose_coordinator_items, choose_dispatcher_items, dump_jsonl
from compat.workflow_adapter import OpenJiuwenWorkflowAdapter


DISPATCHER_KEYS = {
    "semantic": "semantic_candidates",
    "temporal": "temporal_candidates",
    "planner": "planner_candidates",
}
COORDINATOR_KEYS = {
    "result_aware": "result_aware_candidates",
    "dependency": "dependency_candidates",
    "finalizer": "finalizer_candidates",
}


def main() -> None:
    phase = sys.argv[1] if len(sys.argv) > 1 else "dispatcher"
    path_name = sys.argv[2] if len(sys.argv) > 2 else "semantic"
    session_id = current_session_id()
    user_text = current_user_text()
    databus = DataBusBridge()
    session_context = env_text("AF_SESSION_CONTEXT") or databus.get(f"session:{session_id}:context")
    skill_catalog = env_text("AF_SKILL_CATALOG") or databus.get("skill_catalog")
    selected_skills = env_text("AF_SELECTED_SKILLS") or databus.get("selected_skills")
    partial_results = env_text("AF_PARTIAL_SKILL_RESULTS") or databus.get("partial_skill_results")

    adapter = OpenJiuwenWorkflowAdapter(
        f"{phase}-{path_name}-selector",
        f"Select skills for {phase}:{path_name}",
        databus=databus,
    )
    prompt = json.dumps(
        {
            "phase": phase,
            "path": path_name,
            "task": "从技能清单中选择0~N个真正能完成请求的技能，并给出优先级和状态。若没有合适技能，返回空列表。",
            "user_text": user_text,
            "session_context": session_context,
            "skill_catalog": skill_catalog,
            "selected_skills": selected_skills,
            "partial_skill_results": partial_results,
            "rules": [
                "严格遵守每个技能的 handles / does_not_handle / abstain_when 约束。",
                "不要因为技能能输出泛化兜底文本就选择它。",
                "如果问题需要当前示例并不具备的工具能力，应返回空 skills。",
            ],
        },
        ensure_ascii=False,
    )
    schema = {
        "type": "object",
        "properties": {
            "skills": {
                "type": "array",
                "items": {
                    "type": "object",
                    "properties": {
                        "skill": {"type": "string"},
                        "priority": {"type": "integer"},
                        "status": {"type": "string"},
                        "reason": {"type": "string"},
                    },
                    "required": ["skill"],
                },
            }
        },
    }
    llm_result = adapter.query_reasoner(
        prompt,
        "你是一个技能路由器，只返回适合处理请求的技能建议；不确定时返回空列表。",
        schema,
    )
    if phase == "dispatcher":
        items = choose_dispatcher_items(session_id, user_text, path_name, llm_result.text)
        key = DISPATCHER_KEYS.get(path_name, "candidate_list")
    else:
        items = choose_coordinator_items(session_id, path_name, selected_skills, partial_results, llm_result.text)
        key = COORDINATOR_KEYS.get(path_name, "candidate_list")
    emit_json({key: dump_jsonl(items), "reasoner_source": llm_result.source, "reasoner_error": llm_result.error})


if __name__ == "__main__":
    main()
