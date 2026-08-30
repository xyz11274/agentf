"""Tests for the repository credential scanner."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path


_MODULE_PATH = Path(__file__).parents[1] / "quality" / "scan_secrets.py"
_SPEC = importlib.util.spec_from_file_location("agentflow_secret_scanner", _MODULE_PATH)
assert _SPEC is not None and _SPEC.loader is not None
_SCANNER = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = _SCANNER
_SPEC.loader.exec_module(_SCANNER)


def test_detects_arbitrary_provider_credential_assignment():
    value = "Nv7p" + "Q2m8Z4r6T1x9L5c3"
    findings = _SCANNER.scan_bytes(
        "config/provider.yaml",
        f"NOVA_CREDENTIAL={value}\n".encode(),
    )

    assert [(item.rule, item.label) for item in findings] == [
        ("credential_assignment", "NOVA_CREDENTIAL"),
    ]


def test_explicit_custom_name_covers_unusual_provider_variable():
    value = "R8v2" + "N6q4P9w7K3z5"
    findings = _SCANNER.scan_bytes(
        "config/provider.env.example",
        f"VENDOR_LOGIN={value}\n".encode(),
        custom_names={"VENDOR_LOGIN"},
    )

    assert findings[0].rule == "credential_assignment"
    assert findings[0].label == "VENDOR_LOGIN"


def test_camelcase_session_and_refresh_tokens_are_detected():
    content = (
        "sessionToken: opaque-session-value-998877\n"
        "refreshToken: opaque-refresh-value-998877\n"
    ).encode()

    findings = _SCANNER.scan_bytes("config/provider.yaml", content)

    assert {item.label for item in findings} == {"sessionToken", "refreshToken"}


def test_repo_declared_custom_names_apply_without_ci_environment():
    value = "R8v2" + "N6q4P9w7K3z5"
    targets = [
        ("config/template.env", b"AF_SECRET_ENV_NAMES=VENDOR_LOGIN\n"),
        ("config/provider.env", f"VENDOR_LOGIN={value}\n".encode()),
    ]

    custom_names = _SCANNER._declared_custom_names(targets)
    findings = _SCANNER.scan_bytes(
        targets[1][0], targets[1][1], custom_names=custom_names,
    )

    assert custom_names == {"VENDOR_LOGIN"}
    assert [(item.rule, item.label) for item in findings] == [
        ("credential_assignment", "VENDOR_LOGIN"),
    ]


def test_provider_prefix_is_caught_under_innocuous_name():
    value = "sk-" + "livevalue1234567890ABCDE"
    findings = _SCANNER.scan_bytes(
        "config/settings.yaml",
        f"opaque_value: {value}\n".encode(),
    )

    assert findings[0].rule == "credential_prefix"


def test_placeholders_references_and_test_sentinels_are_allowed():
    content = b"""\
API_KEY=${API_KEY}
TOKEN=vault:team/service
client_secret=<credential-placeholder>
password=test-password-not-real
AF_MAX_COMPLETION_TOKENS=8192
"""
    assert _SCANNER.scan_bytes("config/example.env", content) == []


def test_operational_token_metrics_are_not_credentials():
    value = "V9m2" + "Q7r4L8x6"
    content = (
        "token_usage=8192\n"
        "token_delay=0.01\n"
        "parse_token=result\n"
        f"api_token={value}\n"
    ).encode()

    findings = _SCANNER.scan_bytes("config/runtime.yaml", content)

    assert [(item.rule, item.label) for item in findings] == [
        ("credential_assignment", "api_token"),
    ]


def test_detects_literal_fallback_inside_environment_lookup():
    value = "F8q2" + "R7m4V9x6L3z1"
    source = f'API_KEY = os.environ.get("API_KEY", "{value}")\n'

    findings = _SCANNER.scan_bytes("src/provider_config.py", source.encode())

    assert [(item.rule, item.label) for item in findings] == [
        ("credential_assignment", "API_KEY"),
    ]


def test_credential_metadata_names_and_environment_reference_are_allowed():
    source = b'''\
_CREDENTIAL_FILES = frozenset({".env", "credentials.json"})
api_key_len = len(api_key)
API_KEY = os.environ.get("API_KEY", "")
'''

    assert _SCANNER.scan_bytes("src/provider_config.py", source) == []


def test_auth_url_and_private_key_are_high_signal_findings():
    auth_value = "Bearer " + "QWERTY1234567890"
    credential_url = "https://user:long-" + "password@example.test/api"
    private_key_marker = "-----BEGIN " + "PRIVATE KEY-----"
    content = (
        f"Authorization: {auth_value}\n"
        f"endpoint={credential_url}\n"
        f"{private_key_marker}\n"
    ).encode()

    rules = {
        item.rule
        for item in _SCANNER.scan_bytes("config/runtime.txt", content)
    }
    assert rules == {"authorization_literal", "private_key_block", "url_userinfo"}


def test_finding_output_never_contains_value_or_value_hash():
    value = "K7m2" + "Z9q4R8v6T3x1"
    findings = _SCANNER.scan_bytes(
        "config/provider.env",
        f"ACME_SECRET={value}\n".encode(),
    )

    rendered = _SCANNER.format_findings(findings)

    assert value not in rendered
    assert "ACME_SECRET" in rendered
    assert "credential_assignment" in rendered


def test_binary_files_are_ignored():
    assert _SCANNER.scan_bytes("assets/image.png", b"\x89PNG\x00API_KEY=hidden") == []
