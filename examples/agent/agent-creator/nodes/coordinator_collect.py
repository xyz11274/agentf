#!/usr/bin/env python3
"""Coordinator Collect Node — minimal pass-through for the coordinator.

The skill (harness_run) has already written the LLM response to DataBus
via its node outputs (json_output → process_node → publish_data).
This node exists only to satisfy the framework's workflow lifecycle.
"""
import json


def execute():
    print(json.dumps({"coordinator_status": "ok"}, ensure_ascii=False))


if __name__ == "__main__":
    execute()
