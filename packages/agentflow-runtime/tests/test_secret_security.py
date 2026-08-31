"""Credential isolation regression tests."""

from __future__ import annotations

import base64
import hashlib
import io
import json
import urllib.parse

import pytest

from agentflow_runtime.security import (
    SecretStreamRedactor,
    RedactingTextIO,
    find_registered_secret_exposures,
    find_secret_exposures,
    redact_secrets,
    register_environment_secrets,
    register_secret,
    safe_subprocess_environment,
)
from agentflow_runtime.security.secrets import _reset_registry_for_tests


@pytest.fixture(autouse=True)
def _isolated_secret_registry():
    _reset_registry_for_tests()
    yield
    _reset_registry_for_tests()


def test_registered_secret_redacts_common_transport_representations():
    secret = 'vendor-"A/B+C?=7&scope=x-998877'
    assert register_secret("provider.odd_credential", secret)

    variants = {
        secret,
        urllib.parse.quote(secret, safe=""),
        urllib.parse.quote_plus(secret, safe=""),
        base64.b64encode(secret.encode()).decode(),
        base64.urlsafe_b64encode(secret.encode()).decode(),
        json.dumps(secret, ensure_ascii=False)[1:-1],
    }
    redacted, report = redact_secrets({"values": sorted(variants)})
    serialized = json.dumps(redacted, ensure_ascii=False)

    assert report.applied
    assert report.count >= len(variants)
    assert "provider.odd_credential" in report.labels
    assert all(variant not in serialized for variant in variants)


@pytest.mark.parametrize(
    "name",
    [
        "OPENAI_API_KEY",
        "DASHSCOPE_API_KEY",
        "ARK_API_KEY",
        "HF_TOKEN",
        "AWS_SECRET_ACCESS_KEY",
        "AZURE_CLIENT_SECRET",
        "GOOGLE_APPLICATION_CREDENTIALS",
    ],
)
def test_provider_agnostic_environment_names_are_registered(name):
    secret = f"credential-for-{name}-998877"
    register_environment_secrets({name: secret})

    report = find_registered_secret_exposures({"nested": [secret]})

    assert report.applied
    assert f"env.{name}" in report.labels


def test_explicit_env_name_covers_nonstandard_vendor_name():
    secret = "nonstandard-provider-value-998877"
    register_environment_secrets({
        "AF_SECRET_ENV_NAMES": "ODD_VENDOR_HANDLE",
        "ODD_VENDOR_HANDLE": secret,
    })

    assert find_registered_secret_exposures(secret).applied


def test_generic_authorization_assignment_and_private_key_are_redacted():
    private_key = (
        "-----BEGIN " + "PRIVATE KEY-----\n"
        "not-a-real-private-key-but-long-enough\n"
        "-----END PRIVATE KEY-----"
    )
    payload = {
        "header": "Authorization" + ": Bearer " + "opaque-provider-token-998877",
        "config": "client_secret=opaque-client-value-998877",
        "key": private_key,
    }

    redacted, report = redact_secrets(payload)
    serialized = json.dumps(redacted)

    assert report.applied
    assert "opaque-provider-token-998877" not in serialized
    assert "opaque-client-value-998877" not in serialized
    assert "not-a-real-private-key" not in serialized


def test_structured_credential_fields_redact_arbitrary_formats():
    values = {
        "apiKey": "opaque value with spaces and punctuation !?",
        "AccessKeyId": "id-7",
        "sessionToken": "session-7",
        "nested": {
            "provider_client_secret": "xy-7",
            "password": 1234,
        },
        "api_key_configured": True,
        "empty_access_token": "",
        "messages": [
            "authToken=unregistered-opaque-value-998877",
            "aws_access_key_id=unregistered-access-value-998877",
            "session_token=unregistered-session-value-998877",
        ],
    }

    redacted, report = redact_secrets(values)
    serialized = json.dumps(redacted, ensure_ascii=False)

    assert report.applied
    assert "opaque value with spaces" not in serialized
    assert "xy-7" not in serialized
    assert "id-7" not in serialized
    assert "session-7" not in serialized
    assert "1234" not in serialized
    assert "unregistered-opaque-value-998877" not in serialized
    assert "unregistered-access-value-998877" not in serialized
    assert "unregistered-session-value-998877" not in serialized
    assert redacted["api_key_configured"] is True
    assert redacted["empty_access_token"] == ""


def test_stream_redactor_never_releases_secret_split_across_chunks():
    secret = "split-stream-secret-value-998877665544"
    register_secret("provider.stream", secret)
    stream = SecretStreamRedactor()

    chunks = [
        "prefix:",
        secret[:3],
        secret[3:17],
        secret[17:29],
        secret[29:],
        ":suffix",
    ]
    released = "".join(stream.feed(chunk) for chunk in chunks) + stream.flush()

    assert secret not in released
    assert "<redacted:provider.stream>" in released
    assert released.startswith("prefix:")
    assert released.endswith(":suffix")
    assert stream.report.applied

def test_stream_redactor_releases_unrelated_chunks_without_leaking_prefix():
    secret = "prefix-buffer-secret-value-112233445566"
    register_secret("provider.prefix_buffer", secret)
    stream = SecretStreamRedactor()

    assert stream.feed("你好") == "你好"
    assert stream.feed(secret[:7]) == ""

    released = stream.feed(secret[7:])

    assert secret not in released
    assert released == "<redacted:provider.prefix_buffer>"
    assert stream.flush() == ""
    assert stream.report.applied



def test_redaction_report_contains_no_secret_or_secret_hash():
    secret = "report-must-not-identify-secret-998877"
    register_secret("provider.report", secret)

    _value, report = redact_secrets({"token": secret})
    report_json = json.dumps(report.to_dict(), sort_keys=True)

    assert secret not in report_json
    assert hashlib.sha256(secret.encode()).hexdigest() not in report_json
    assert set(report.to_dict()) == {
        "applied", "policy", "count", "labels", "paths",
    }


def test_safe_subprocess_environment_blocks_secret_names_and_value_aliases():
    secret = "subprocess-secret-value-998877"
    source = {
        "PATH": "/usr/bin:/bin",
        "HOME": "/tmp/agentflow-test-home",
        "OPENAI_API_KEY": secret,
        "UNRELATED_SETTING": "not-forwarded",
    }

    child_env = safe_subprocess_environment(
        source,
        extra={
            "RENAMED_VALUE": secret,
            "PUBLIC_SETTING": "safe-value",
            "ANOTHER_TOKEN": "another-secret-value-998877",
        },
    )

    assert child_env["PATH"] == "/usr/bin:/bin"
    assert child_env["HOME"] == "/tmp/agentflow-test-home"
    assert child_env["PUBLIC_SETTING"] == "safe-value"
    assert secret not in child_env.values()
    assert "OPENAI_API_KEY" not in child_env
    assert "RENAMED_VALUE" not in child_env
    assert "ANOTHER_TOKEN" not in child_env


def test_artifact_capability_signature_is_redacted_from_url():
    signature = "a" * 64
    url = f"/api/artifacts/agent/files/output.png?sig={signature}"

    redacted, report = redact_secrets({"delivered_content": url})

    assert report.applied
    assert signature not in redacted["delivered_content"]
    assert "?sig=<redacted:credential_field>" in redacted["delivered_content"]
    assert "/api/artifacts/agent/files/output.png" in redacted["delivered_content"]

def test_repeated_redaction_preserves_nested_json_escaping():
    signature = "a" * 64
    nested_input = json.dumps({
        "messages": [
            {
                "role": "assistant",
                "content": f"/api/artifacts/agent/output.png?sig={signature}",
            },
            {"role": "user", "content": "next turn"},
        ],
    }, ensure_ascii=False)

    safe_event, first_report = redact_secrets({
        "data": {"input": nested_input},
    })
    serialized = json.dumps(
        safe_event, ensure_ascii=False, separators=(",", ":"),
    )
    safe_line, second_report = redact_secrets(serialized)

    assert first_report.applied
    assert not second_report.applied
    assert signature not in safe_line
    assert json.loads(safe_line) == safe_event



def test_stderr_wrapper_redacts_credentials_split_across_writes():
    secret = "stderr-split-test-credential-998877"
    register_secret("test.stderr", secret)
    target = io.StringIO()
    stream = RedactingTextIO(target)

    stream.write("provider failed: ")
    stream.write(secret[:12])
    stream.write(secret[12:])
    stream.write("\n")
    stream.flush()

    output = target.getvalue()
    assert secret not in output
    assert "<redacted:test.stderr>" in output


def test_ordinary_configuration_is_not_over_redacted():
    value = {
        "model": "glm-5.3",
        "endpoint": "https://example.invalid/v1",
        "temperature": 0.2,
    }

    redacted, report = redact_secrets(value)

    assert redacted == value
    assert not report.applied
    assert not find_secret_exposures(value).applied
