"""Bridge 最终交付 SessionLog 测试。"""

from __future__ import annotations

import json

import services.sessionlog_delivery as delivery
from services.sessionlog_delivery import append_bridge_delivery


def test_disabled_when_sessionlog_file_does_not_exist(tmp_path):
    result = append_bridge_delivery(
        agent_package_dir=tmp_path / "agent",
        dialogue_id="d1",
        agent_id="agent",
        transport="sse",
        stage="server_yielded",
        status="final",
        raw_content="raw",
        delivered_content="public",
        wire_payload={"_final": True},
    )
    assert result is False
    assert not (tmp_path / "agent" / "trace_output").exists()


def test_appends_exact_delivery_event(tmp_path):
    agent_dir = tmp_path / "agent"
    output_dir = agent_dir / "trace_output" / "sessionlogs"
    output_dir.mkdir(parents=True)
    path = output_dir / "d1.jsonl"
    candidate = {
        "type": "session.end",
        "event_id": "python:1",
        "data": {"response_text": "raw"},
    }
    path.write_text(
        '{"type":"session.header"}\n'
        + json.dumps(candidate, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    wire_payload = {"choices": [{"message": {"content": "public"}}], "_final": True}
    result = append_bridge_delivery(
        agent_package_dir=agent_dir,
        dialogue_id="d1",
        agent_id="agent",
        session_id="s1",
        task_id="t1",
        transport="sse",
        stage="server_yielded",
        status="final",
        raw_content="raw",
        delivered_content="public",
        wire_payload=wire_payload,
        artifacts=[{"name": "look.png"}],
    )
    assert result is True
    records = [
        json.loads(line)
        for line in path.read_text(encoding="utf-8").splitlines()
    ]
    event = records[-1]
    assert event["type"] == "client.delivery"
    assert event["producer_id"].startswith("bridge:")
    assert event["session_id"] == "s1"
    assert event["task_id"] == "t1"
    assert event["data"]["raw_content"] == "raw"
    assert event["data"]["delivered_content"] == "public"
    assert event["data"]["wire_payload"] == wire_payload
    assert event["data"]["client_acknowledged"] is False
    assert event["data"]["raw_content_sha256"] != event["data"]["delivered_content_sha256"]
    assert event["primary_parent_event_id"] == "python:1"
    assert event["causal_parent_event_ids"] == ["python:1"]
    assert event["data"]["candidate_event_id"] == "python:1"
    assert event["data"]["candidate_matches_raw_content"] is True


def test_finds_daemon_project_trace_without_bridge_trace_env(tmp_path, monkeypatch):
    """Bridge 未继承 daemon 环境时仍能定位仓库级 SessionLog。"""
    monkeypatch.delenv("AF_SESSIONLOG_DIR", raising=False)
    monkeypatch.delenv("AF_TRACE_OUTPUT_DIR", raising=False)
    monkeypatch.setattr(delivery, "_PROJECT_ROOT", tmp_path / "project")
    output_dir = tmp_path / "project" / "trace_output" / "sessionlogs"
    output_dir.mkdir(parents=True)
    path = output_dir / "d1.jsonl"
    path.write_text('{"type":"session.header"}\n', encoding="utf-8")

    assert append_bridge_delivery(
        agent_package_dir=tmp_path / "resource" / "agent-packages" / "agent",
        dialogue_id="d1",
        agent_id="agent",
        transport="http_json",
        stage="response_prepared",
        status="final",
        raw_content="raw",
        delivered_content="public",
        wire_payload={"content": "public"},
    ) is True
    assert json.loads(
        path.read_text(encoding="utf-8").splitlines()[-1]
    )["type"] == "client.delivery"


def test_event_build_failure_is_fail_open(tmp_path, monkeypatch):
    agent_dir = tmp_path / "agent"
    output_dir = agent_dir / "trace_output" / "sessionlogs"
    output_dir.mkdir(parents=True)
    (output_dir / "d1.jsonl").write_text(
        '{"type":"session.header"}\n', encoding="utf-8",
    )

    def fail_hash(_value):
        raise TypeError("not serializable")

    monkeypatch.setattr(delivery, "_canonical_sha256", fail_hash)
    assert delivery.append_bridge_delivery(
        agent_package_dir=agent_dir,
        dialogue_id="d1",
        agent_id="agent",
        transport="http_json",
        stage="response_prepared",
        status="final",
        raw_content="raw",
        delivered_content="public",
        wire_payload={"content": "public"},
    ) is False
