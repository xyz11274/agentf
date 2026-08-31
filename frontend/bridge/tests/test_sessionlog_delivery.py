"""Bridge 最终交付轨迹日志测试。"""

from __future__ import annotations

import json
import logging

import services.secret_safety as secret_safety

import services.sessionlog_delivery as delivery
from services.sessionlog_delivery import append_bridge_delivery


def test_log_filter_redacts_messages_tracebacks_and_fails_closed(monkeypatch):
    secret = "bridge-log-test-credential-998877"
    monkeypatch.setenv("BRIDGE_TEST_API_KEY", secret)
    log_filter = secret_safety.CredentialRedactionFilter()
    exc = ValueError(f"provider rejected {secret}")
    record = logging.LogRecord(
        "bridge.test", logging.ERROR, __file__, 1,
        "request api_key=%s", (secret,), (ValueError, exc, None),
    )

    assert log_filter.filter(record) is True
    assert secret not in record.getMessage()
    assert record.args == ()
    assert record.exc_info is None

    monkeypatch.setattr(secret_safety, "_redact_secrets", None)
    unavailable = logging.LogRecord(
        "bridge.test", logging.ERROR, __file__, 1, secret, (), None,
    )
    assert log_filter.filter(unavailable) is True
    assert unavailable.getMessage() == "<redacted:log-record>"


def test_log_filter_preserves_uvicorn_access_formatter_contract(monkeypatch):
    from uvicorn.logging import AccessFormatter

    secret = "bridge-access-test-credential-998877"
    monkeypatch.setenv("BRIDGE_ACCESS_API_KEY", secret)
    log_filter = secret_safety.CredentialRedactionFilter()
    formatter = AccessFormatter(
        '%(client_addr)s - "%(request_line)s" %(status_code)s',
        use_colors=False,
    )
    record = logging.LogRecord(
        "uvicorn.access", logging.INFO, __file__, 1,
        '%s - "%s %s HTTP/%s" %d',
        ("127.0.0.1:8199", "GET", f"/chat?api_key={secret}", "1.1", 200),
        None,
    )

    assert log_filter.filter(record) is True
    assert isinstance(record.args, tuple) and len(record.args) == 5
    rendered = formatter.format(record)
    assert secret not in rendered
    assert "GET" in rendered and "200 OK" in rendered

    monkeypatch.setattr(secret_safety, "_redact_secrets", None)
    unavailable = logging.LogRecord(
        "uvicorn.access", logging.INFO, __file__, 1,
        '%s - "%s %s HTTP/%s" %d',
        ("127.0.0.1:8199", "GET", f"/chat?api_key={secret}", "1.1", 200),
        None,
    )
    assert log_filter.filter(unavailable) is True
    fail_closed_rendered = formatter.format(unavailable)
    assert secret not in fail_closed_rendered
    assert "<redacted>" in fail_closed_rendered


def test_log_filter_preserves_uvicorn_error_message_parameters(monkeypatch):
    from uvicorn.logging import DefaultFormatter

    secret = "bridge-uvicorn-error-credential-998877"
    monkeypatch.setenv("BRIDGE_UVICORN_API_KEY", secret)
    record = logging.LogRecord(
        "uvicorn.error", logging.INFO, __file__, 1,
        "Uvicorn running on %s://%s:%d?api_key=%s",
        ("http", "127.0.0.1", 8199, secret), None,
    )
    record.__dict__["color_message"] = (
        "Uvicorn running on \x1b[1m%s://%s:%d?api_key=%s\x1b[0m"
    )

    assert secret_safety.CredentialRedactionFilter().filter(record) is True
    rendered = DefaultFormatter("%(message)s", use_colors=True).format(record)
    assert secret not in rendered
    assert "%s" not in rendered and "%d" not in rendered
    assert "127.0.0.1" in rendered and "8199" in rendered


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
    path = output_dir / "by-user" / "user-a" / "client-a" / "d1.jsonl"
    path.parent.mkdir(parents=True)
    candidate = {
        "type": "dialogue.end",
        "event_id": "python:1",
        "data": {"response_text": "raw"},
    }
    path.write_text(
        '{"type":"dialogue.header"}\n'
        + json.dumps(candidate, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    wire_payload = {"choices": [{"message": {"content": "public"}}], "_final": True}
    result = append_bridge_delivery(
        agent_package_dir=agent_dir,
        dialogue_id="d1",
        agent_id="agent",
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
    assert event["schema_version"] == 2
    assert event["producer_id"].startswith("bridge:")
    assert "session_id" not in event
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
    assert not (output_dir / "d1.jsonl").exists()


def test_identity_route_does_not_cross_write_same_dialogue_id(
    tmp_path, monkeypatch
):
    agent_dir = tmp_path / "agent"
    output_dir = agent_dir / "trace_output" / "sessionlogs"
    monkeypatch.setattr(delivery, "_PROJECT_ROOT", tmp_path / "project")
    dialogue_id = "shared-dialogue"
    paths = {
        "a": output_dir / "by-user" / "user-a" / "client-a"
        / f"{dialogue_id}.jsonl",
        "b": output_dir / "by-user" / "user-b" / "client-b"
        / f"{dialogue_id}.jsonl",
    }
    for label, path in paths.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            '{"type":"dialogue.header"}\n'
            + json.dumps({
                "type": "dialogue.end",
                "event_id": f"python:{label}",
                "data": {"response_text": f"raw-{label}"},
            })
            + "\n",
            encoding="utf-8",
        )
    original_a = paths["a"].read_bytes()

    assert append_bridge_delivery(
        agent_package_dir=agent_dir,
        dialogue_id=dialogue_id,
        agent_id="agent",
        user_id="user-b",
        client_id="client-b",
        transport="http_json",
        stage="response_prepared",
        status="final",
        raw_content="raw-b",
        delivered_content="public-b",
        wire_payload={"content": "public-b"},
    ) is True

    assert paths["a"].read_bytes() == original_a
    record_b = json.loads(paths["b"].read_text(encoding="utf-8").splitlines()[-1])
    assert record_b["type"] == "client.delivery"
    assert record_b["primary_parent_event_id"] == "python:b"

    original_b = paths["b"].read_bytes()
    assert append_bridge_delivery(
        agent_package_dir=agent_dir,
        dialogue_id=dialogue_id,
        agent_id="agent",
        user_id="user-c",
        client_id="client-c",
        transport="http_json",
        stage="response_prepared",
        status="final",
        raw_content="raw-c",
        delivered_content="public-c",
        wire_payload={"content": "public-c"},
    ) is False
    assert paths["a"].read_bytes() == original_a
    assert paths["b"].read_bytes() == original_b


def test_finds_daemon_project_trace_without_bridge_trace_env(tmp_path, monkeypatch):
    """Bridge 未继承 daemon 环境时仍能定位仓库级 SessionLog。"""
    monkeypatch.delenv("AF_SESSIONLOG_DIR", raising=False)
    monkeypatch.delenv("AF_TRACE_OUTPUT_DIR", raising=False)
    monkeypatch.setattr(delivery, "_PROJECT_ROOT", tmp_path / "project")
    output_dir = tmp_path / "project" / "trace_output" / "sessionlogs"
    output_dir.mkdir(parents=True)
    path = output_dir / "d1.jsonl"
    path.write_text('{"type":"dialogue.header"}\n', encoding="utf-8")

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
        '{"type":"dialogue.header"}\n', encoding="utf-8",
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


def test_latest_dialogue_end_accepts_legacy_session_event(tmp_path):
    """历史 session.end 仍可作为 client.delivery 的候选父事件。"""
    path = tmp_path / "legacy.jsonl"
    path.write_text(
        '{"type":"session.end","event_id":"legacy:1",'
        '"data":{"response_text":"legacy"}}\n',
        encoding="utf-8",
    )

    assert delivery._latest_dialogue_end(path) == ("legacy:1", "legacy")

def test_delivery_sink_redacts_secret_before_content_hashing(tmp_path, monkeypatch):
    import base64
    import hashlib

    secret = "bridge-provider-secret/A+B-998877665544"
    encoded = base64.b64encode(secret.encode()).decode()
    monkeypatch.setenv("ODD_VENDOR_TOKEN", secret)

    agent_dir = tmp_path / "agent"
    path = (
        agent_dir / "trace_output" / "sessionlogs"
        / "by-user" / "user-a" / "client-a" / "d1.jsonl"
    )
    path.parent.mkdir(parents=True)
    path.write_text(
        '{"type":"dialogue.header"}\n'
        '{"type":"dialogue.end","event_id":"python:1",'
        '"data":{"response_text":"safe"}}\n',
        encoding="utf-8",
    )

    assert append_bridge_delivery(
        agent_package_dir=agent_dir,
        dialogue_id="d1",
        agent_id="agent",
        transport="sse",
        stage="server_yielded",
        status="final",
        raw_content=secret,
        delivered_content=f"result {secret}",
        wire_payload={"content": encoded, "_final": True},
    ) is True

    record_text = path.read_text(encoding="utf-8").splitlines()[-1]
    record = json.loads(record_text)
    assert secret not in record_text
    assert encoded not in record_text
    assert hashlib.sha256(secret.encode()).hexdigest() not in record_text
    assert record["redaction"]["applied"] is True
    assert "<redacted:env.ODD_VENDOR_TOKEN>" in record_text

    safe_component = delivery._safe_path_component(secret, "unknown")
    assert secret not in safe_component
    assert hashlib.sha256(secret.encode()).hexdigest()[:12] not in safe_component


def test_delivery_sink_writes_nothing_when_sanitizer_is_unavailable(
    tmp_path, monkeypatch
):
    from services.secret_safety import SecretSanitizerUnavailable

    agent_dir = tmp_path / "agent"
    path = agent_dir / "trace_output" / "sessionlogs" / "d1.jsonl"
    path.parent.mkdir(parents=True)
    original = b'{"type":"dialogue.header"}\n'
    path.write_bytes(original)

    def unavailable(_value):
        raise SecretSanitizerUnavailable("unavailable")

    monkeypatch.setattr(delivery, "sanitize_for_storage", unavailable)
    assert append_bridge_delivery(
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
    assert path.read_bytes() == original
