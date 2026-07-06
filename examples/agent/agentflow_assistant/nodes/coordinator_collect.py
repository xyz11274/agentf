#!/usr/bin/env python3
"""
Coordinator Collect Node — minimal pass-through for the coordinator.

The skill (agentflow_assistant_run) has already written the LLM response to
DataBus via its node outputs. This node exists only to satisfy the framework's
workflow lifecycle — once it completes, the runtime sends a "workflow_completed"
message to the coordinator's data_exchange queue.

Outputs coordinator_status to avoid overwriting the skill's "response" key.
"""

import json


def execute():
    print(json.dumps({
        "coordinator_status": "ok",
    }, ensure_ascii=False))


if __name__ == "__main__":
    execute()
