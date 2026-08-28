"""Harness Workflow JSON 与 Python stdout ABI 的契约测试。"""

from __future__ import annotations

import json
from pathlib import Path

import pytest


PROJECT_ROOT = Path(__file__).resolve().parents[3]
WORKFLOW_ROOTS = [
    PROJECT_ROOT / "examples/agent/outfit-assistant",
    PROJECT_ROOT / "resource/agent-packages/outfit-assistant",
    PROJECT_ROOT / "frontend/bridge/templates/agent_template/harness_agent",
]

SKILL_INPUTS = {
    "input": "string",
    "session_id": "string",
    "dialogue_id": "string",
    "exec_skill_key": "string",
    "batch_id": "string",
}
SKILL_OUTPUTS = {
    "skill_result": "string",
    "status": "string",
    "skill_name": "string",
    "rounds_used": "int",
    "artifacts_json": "string",
    "error": "string",
}
COORDINATOR_OUTPUTS = {
    "response": "string",
    "status": "string",
}


def _fields(node: dict, name: str) -> dict[str, str]:
    return {field["name"]: field["type"] for field in node[name]}


@pytest.mark.parametrize("workflow_root", WORKFLOW_ROOTS)
def test_skill_workflow_matches_executor_stdout_contract(workflow_root: Path):
    workflow = json.loads((workflow_root / "skill_workflow.json").read_text())
    node = workflow["nodes"][0]

    assert _fields(node, "inputs") == SKILL_INPUTS
    assert _fields(node, "outputs") == SKILL_OUTPUTS
    assert "response" not in _fields(node, "outputs")


@pytest.mark.parametrize("workflow_root", WORKFLOW_ROOTS)
def test_coordinator_workflow_declares_only_stdout_outputs(workflow_root: Path):
    workflow = json.loads((workflow_root / "coordinator_workflow.json").read_text())
    outputs = _fields(workflow["nodes"][0], "outputs")

    assert outputs == COORDINATOR_OUTPUTS
    assert "next_skills" not in outputs
    assert "coordinator_status" not in outputs
