#!/usr/bin/env python3
"""
Coordinator Collect — bridges skill completion back to coordinator-skill.

When a skill completes, the framework triggers the coordinator task which runs
this node. This node:

1. Reads the skill result from DataBus (skill_result:{name})
2. Appends the skill result to the coordinator's conversation history
3. Outputs next_skills=["coordinator-skill"] with force=true to re-trigger
   the coordinator-skill for another round of LLM decision-making

If no skill completed (edge case), outputs coordinator_status="ok" as fallback.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from databus_client import DataBusClient


def execute():
    completed_skill = os.environ.get("AF_COMPLETED_SKILL_NAME", "")
    skill_success = os.environ.get("AF_SKILL_SUCCESS", "")
    dialogue_id = os.environ.get("AF_DIALOGUE_ID", "")
    session_id = os.environ.get("AF_SESSION_ID", "")

    databus = DataBusClient()

    # coordinator-skill completing means it output "response" to DataBus.
    # Do NOT re-trigger — let framework read "response" and return to client.
    if completed_skill == "coordinator-skill":
        print(json.dumps({"coordinator_status": "ok"}, ensure_ascii=False))
        return

    if completed_skill and databus.is_connected():
        # Read skill result from DataBus
        raw = databus.get(f"skill_result:{completed_skill}")
        skill_result_text = ""
        if raw:
            try:
                skill_data = raw if isinstance(raw, dict) else json.loads(raw)
                skill_result_text = skill_data.get("skill_result", str(raw))
            except (json.JSONDecodeError, TypeError):
                skill_result_text = str(raw)

        # Append skill result to coordinator conversation history
        history = databus.read_messages()
        history.append({
            "role": "user",
            "content": (
                f"Skill '{completed_skill}' 已完成 (success={skill_success})。\n"
                f"结果: {skill_result_text}\n"
                f"请评估结果，决定是否需要再委派 skill 或直接回复用户。"
            )
        })
        databus.write_messages(history)

        # Re-trigger coordinator-skill for another round of decision-making
        # force=true because coordinator-skill was already launched in round 1
        print(json.dumps({
            "next_skills": json.dumps([{"name": "coordinator-skill", "force": True}])
        }, ensure_ascii=False))
    else:
        # Fallback: no skill completed or DataBus unavailable
        print(json.dumps({"coordinator_status": "ok"}, ensure_ascii=False))


if __name__ == "__main__":
    execute()
