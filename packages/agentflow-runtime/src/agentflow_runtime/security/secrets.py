"""Source-aware secret registration, redaction and outbound blocking.

Secret formats are intentionally not tied to a provider.  Trusted code
registers the value when it loads a credential; every later boundary matches
that exact value (and common transport encodings).  Generic key-shaped
patterns are only a defence-in-depth fallback.
"""

from __future__ import annotations

import base64
import json
import os
import re
import threading
import urllib.parse
from dataclasses import dataclass
from typing import Any, Mapping


_LOCK = threading.RLock()
_SECRETS: dict[str, str] = {}
_MIN_VALUE_LENGTH = 8
_PLACEHOLDER_RE = re.compile(r"<redacted:[A-Za-z0-9_.-]+>")

_SECRET_NAME_RE = re.compile(
    r"(?:^|[_-])(?:"
    r"api[_-]?key|token|secret|password|passwd|credentials?|"
    r"private[_-]?key|access[_-]?key|client[_-]?secret|"
    r"signing[_-]?key|auth[_-]?key|cookie"
    r")(?:$|[_-])",
    re.IGNORECASE,
)

_SECRET_FIELD_RE = re.compile(
    r"(?:^|[_-])(?:"
    r"api[_-]?key|access[_-]?(?:key(?:[_-]?id)?|token)|refresh[_-]?token|"
    r"client[_-]?secret|private[_-]?key|signing[_-]?key|"
    r"auth[_-]?(?:key|token)|bearer[_-]?token|session[_-]?token|secret|password|passwd|"
    r"credentials?|authorization|cookie|signature|sig"
    r")$",
    re.IGNORECASE,
)
_CAMEL_SECRET_FIELD_RE = re.compile(
    r"(?:apiKey|accessKey|accessKeyId|accessToken|refreshToken|sessionToken|clientSecret|privateKey|"
    r"signingKey|authKey|authToken|bearerToken|signature)$",
    re.IGNORECASE,
)

_ASSIGNMENT_RE = re.compile(
    r"(?i)(?P<prefix>"
    r"(?:[\"']?(?:api[_-]?key|access[_-]?(?:key(?:[_-]?id)?|token)|refresh[_-]?token|session[_-]?token|"
    r"client[_-]?secret|private[_-]?key|signing[_-]?key|password|passwd|"
    r"credentials?|authorization|cookie|apiKey|accessKey|accessKeyId|accessToken|"
    r"refreshToken|sessionToken|clientSecret|privateKey|signingKey|authKey|authToken|signature|sig)[\"\x27]?)"
    r"\s*(?:=|:)\s*[\"']?"
    r")(?P<value>[^\s\"',;\}\]]{8,})(?P<suffix>[\"']?)"
)
_CLI_SECRET_RE = re.compile(
    r"(?i)(?P<prefix>--(?:api[_-]?key|token|secret|password|credentials?)\s+)"
    r"(?P<value>[^\s\"']{8,})"
)
_AUTH_RE = re.compile(
    r"(?i)(?P<prefix>(?:authorization\s*[:=]\s*)?(?:bearer|basic)\s+)"
    r"(?P<value>[A-Za-z0-9._~+/=-]{8,})"
)
_PEM_RE = re.compile(
    r"-----BEGIN (?:[A-Z0-9 ]+ )?PRIVATE KEY-----.*?"
    r"-----END (?:[A-Z0-9 ]+ )?PRIVATE KEY-----",
    re.DOTALL,
)
_PROVIDER_KEY_RE = re.compile(
    r"(?<![A-Za-z0-9])(?P<value>(?:"
    r"(?:sk|ark)-[A-Za-z0-9_-]{16,}|hf_[A-Za-z0-9]{20,}|"
    r"gh[pousr]_[A-Za-z0-9]{20,}|xox[baprs]-[A-Za-z0-9-]{16,}|"
    r"AKIA[0-9A-Z]{16}|AIza[0-9A-Za-z_-]{20,}"
    r"))(?![A-Za-z0-9])"
)


_SAFE_ENV_NAMES = frozenset({
    "PATH", "PYTHONPATH", "VIRTUAL_ENV", "CONDA_PREFIX", "CONDA_DEFAULT_ENV",
    "LANG", "LC_ALL", "LC_CTYPE", "TZ", "TERM", "TMPDIR", "TEMP", "TMP",
    "HOME", "USER", "LOGNAME", "SHELL", "SSL_CERT_FILE", "SSL_CERT_DIR",
    "REQUESTS_CA_BUNDLE", "CURL_CA_BUNDLE", "NO_PROXY", "no_proxy",
})


def _safe_label(name: str) -> str:
    label = re.sub(r"[^A-Za-z0-9_.-]+", "_", str(name or "secret")).strip("._")
    return label[:96] or "secret"


def _is_usable_secret(value: Any) -> bool:
    if not isinstance(value, str) or len(value) < _MIN_VALUE_LENGTH:
        return False
    return value.strip().lower() not in {
        "true", "false", "none", "null", "undefined", "password", "changeme",
    }


def register_secret(name: str, value: Any) -> bool:
    """Register an exact credential value without assuming a vendor format."""
    if not _is_usable_secret(value):
        return False
    label = _safe_label(name)
    with _LOCK:
        _SECRETS[label] = value
    return True


def _is_secret_name(name: str) -> bool:
    normalized = str(name or "").strip()
    return bool(_SECRET_NAME_RE.search(normalized))


def _is_secret_field_name(name: str) -> bool:
    """Recognize credential-bearing structured fields, including camelCase."""
    normalized = str(name or "").strip()
    return bool(_SECRET_FIELD_RE.search(normalized)
                or _CAMEL_SECRET_FIELD_RE.search(normalized))


def register_environment_secrets(
    environ: Mapping[str, str] | None = None,
    *,
    extra_names: list[str] | tuple[str, ...] | None = None,
) -> int:
    """Register credential-bearing environment values by source name.

    ``AF_SECRET_ENV_NAMES`` is a comma-separated escape hatch for credentials
    whose variable names do not follow any convention.
    """
    source = os.environ if environ is None else environ
    explicit = {
        item.strip()
        for item in str(source.get("AF_SECRET_ENV_NAMES", "")).split(",")
        if item.strip()
    }
    explicit.update(str(item).strip() for item in (extra_names or ()) if str(item).strip())
    before = len(_SECRETS)
    for name, value in source.items():
        if name in explicit or _is_secret_name(name):
            register_secret(f"env.{name}", value)
    with _LOCK:
        return max(0, len(_SECRETS) - before)


def has_registered_secrets() -> bool:
    register_environment_secrets()
    with _LOCK:
        return bool(_SECRETS)


def _representations(value: str) -> set[str]:
    variants = {value}
    if len(value) >= _MIN_VALUE_LENGTH:
        raw = value.encode("utf-8")
        variants.update({
            urllib.parse.quote(value, safe=""),
            urllib.parse.quote_plus(value, safe=""),
            base64.b64encode(raw).decode("ascii"),
            base64.urlsafe_b64encode(raw).decode("ascii"),
            json.dumps(value, ensure_ascii=False)[1:-1],
        })
    return {item for item in variants if len(item) >= _MIN_VALUE_LENGTH}


def _secret_snapshot() -> list[tuple[str, str]]:
    with _LOCK:
        items = list(_SECRETS.items())
    expanded: list[tuple[str, str]] = []
    for label, value in items:
        expanded.extend((label, item) for item in _representations(value))
    expanded.sort(key=lambda item: len(item[1]), reverse=True)
    return expanded


@dataclass(frozen=True)
class RedactionReport:
    applied: bool = False
    count: int = 0
    labels: tuple[str, ...] = ()
    paths: tuple[str, ...] = ()

    def to_dict(self) -> dict[str, Any]:
        return {
            "applied": self.applied,
            "policy": "secret-v1",
            "count": self.count,
            "labels": list(self.labels),
            "paths": list(self.paths),
        }


class SecretLeakBlocked(RuntimeError):
    """Raised before an external sink receives credential material."""

    def __init__(self, report: RedactionReport):
        super().__init__("secret material detected at external boundary")
        self.report = report


class _ReportBuilder:
    def __init__(self) -> None:
        self.count = 0
        self.labels: set[str] = set()
        self.paths: set[str] = set()

    def add(self, label: str, path: str, count: int = 1) -> None:
        self.count += max(1, count)
        self.labels.add(_safe_label(label))
        self.paths.add(path or "$")

    def freeze(self) -> RedactionReport:
        return RedactionReport(
            applied=self.count > 0,
            count=self.count,
            labels=tuple(sorted(self.labels)),
            paths=tuple(sorted(self.paths)),
        )


def _redact_registered_text(text: str, path: str, report: _ReportBuilder) -> str:
    output = text
    for label, representation in _secret_snapshot():
        occurrences = output.count(representation)
        if occurrences:
            output = output.replace(representation, f"<redacted:{label}>")
            report.add(label, path, occurrences)
    return output


def _redact_text(text: str, path: str, report: _ReportBuilder) -> str:
    output = _redact_registered_text(text, path, report)

    def _replace_match(match: re.Match[str], label: str) -> str:
        value = match.group("value")
        suffix = match.groupdict().get("suffix") or ""
        escaped_suffix = ""
        if suffix and value.endswith("\\"):
            trailing_backslashes = len(value) - len(value.rstrip("\\"))
            if trailing_backslashes % 2:
                value = value[:-1]
                escaped_suffix = "\\"
        if _PLACEHOLDER_RE.fullmatch(value):
            return match.group(0)
        report.add(label, path)
        return f"{match.group('prefix')}<redacted:{label}>{escaped_suffix}{suffix}"

    output = _PEM_RE.sub(
        lambda match: (
            report.add("private_key", path) or "<redacted:private_key>"
        ),
        output,
    )
    output = _AUTH_RE.sub(lambda match: _replace_match(match, "authorization"), output)
    output = _PROVIDER_KEY_RE.sub(
        lambda match: (
            report.add("provider_key", path) or "<redacted:provider_key>"
        ),
        output,
    )
    output = _CLI_SECRET_RE.sub(lambda match: _replace_match(match, "cli_secret"), output)
    output = _ASSIGNMENT_RE.sub(lambda match: _replace_match(match, "credential_field"), output)
    return output


def redact_secrets(value: Any) -> tuple[Any, RedactionReport]:
    """Recursively redact registered and unmistakably structured secrets."""
    register_environment_secrets()
    report = _ReportBuilder()

    def _walk(item: Any, path: str) -> Any:
        if isinstance(item, str):
            return _redact_text(item, path, report)
        if isinstance(item, bytes):
            redacted = _redact_text(item.decode("utf-8", errors="replace"), path, report)
            return redacted.encode("utf-8")
        if isinstance(item, dict):
            out = {}
            for key, nested in item.items():
                safe_key = (
                    _redact_text(key, f"{path}.<key>", report)
                    if isinstance(key, str) else key
                )
                nested_path = f"{path}.{safe_key}"
                is_empty = (
                    nested is None
                    or isinstance(nested, bool)
                    or (isinstance(nested, (str, bytes, list, tuple, dict))
                        and len(nested) == 0)
                )
                if (isinstance(key, str) and _is_secret_field_name(key)
                        and not is_empty
                        and not (isinstance(nested, str)
                                 and _PLACEHOLDER_RE.fullmatch(nested))):
                    label = f"field.{_safe_label(key)}"
                    report.add(label, nested_path)
                    out[safe_key] = f"<redacted:{label}>"
                else:
                    out[safe_key] = _walk(nested, nested_path)
            return out
        if isinstance(item, list):
            return [_walk(nested, f"{path}[{index}]") for index, nested in enumerate(item)]
        if isinstance(item, tuple):
            return tuple(_walk(nested, f"{path}[{index}]") for index, nested in enumerate(item))
        return item

    redacted = _walk(value, "$")
    return redacted, report.freeze()


class SecretStreamRedactor:
    """Redact text streams without leaking a secret split across chunks.

    Only a trailing prefix of a registered representation stays buffered.
    Therefore no credential prefix is released, while unrelated text can be
    forwarded immediately instead of waiting for the whole stream to finish.
    """

    def __init__(self) -> None:
        register_environment_secrets()
        self._pending = ""
        self._count = 0
        self._labels: set[str] = set()
        self._paths: set[str] = set()

    def _merge_report(self, report: RedactionReport) -> None:
        self._count += report.count
        self._labels.update(report.labels)
        self._paths.update(report.paths)

    @property
    def report(self) -> RedactionReport:
        return RedactionReport(
            applied=self._count > 0,
            count=self._count,
            labels=tuple(sorted(self._labels)),
            paths=tuple(sorted(self._paths)),
        )

    @staticmethod
    def _holdback_length(text: str) -> int:
        """Return the longest suffix that may still complete a secret."""
        longest = 0
        for _label, representation in _secret_snapshot():
            upper = min(len(text), len(representation) - 1)
            for length in range(upper, longest, -1):
                if text.endswith(representation[:length]):
                    longest = length
                    break
        return longest

    def feed(self, text: str) -> str:
        """Return only the prefix that is safe to release immediately."""
        self._pending += str(text)
        safe_pending, report = redact_secrets(self._pending)
        self._pending = safe_pending
        self._merge_report(report)
        holdback = self._holdback_length(self._pending)
        boundary = len(self._pending) - holdback
        released, self._pending = self._pending[:boundary], self._pending[boundary:]
        return released

    def flush(self) -> str:
        """Return the final sanitized suffix and clear the buffer."""
        released, report = redact_secrets(self._pending)
        self._merge_report(report)
        self._pending = ""
        return released


def find_secret_exposures(value: Any) -> RedactionReport:
    """Inspect a value without returning or logging any matched credential."""
    _redacted, report = redact_secrets(value)
    return report


def find_registered_secret_exposures(value: Any) -> RedactionReport:
    """Inspect only exact values registered by trusted credential loaders."""
    register_environment_secrets()
    report = _ReportBuilder()

    def _walk(item: Any, path: str) -> None:
        if isinstance(item, str):
            _redact_registered_text(item, path, report)
            return
        if isinstance(item, bytes):
            text = item.decode("utf-8", errors="replace")
            _redact_registered_text(text, path, report)
            return
        if isinstance(item, dict):
            for key, nested in item.items():
                if isinstance(key, str):
                    _redact_registered_text(key, f"{path}.<key>", report)
                _walk(nested, f"{path}.{key}")
            return
        if isinstance(item, (list, tuple)):
            for index, nested in enumerate(item):
                _walk(nested, f"{path}[{index}]")

    _walk(value, "$")
    return report.freeze()


def safe_subprocess_environment(
    source: Mapping[str, str] | None = None,
    *,
    extra: Mapping[str, str] | None = None,
) -> dict[str, str]:
    """Build a strict, secret-free environment for untrusted subprocesses."""
    env_source = os.environ if source is None else source
    register_environment_secrets(env_source)
    result = {}
    for name, value in env_source.items():
        if name not in _SAFE_ENV_NAMES or _is_secret_name(name):
            continue
        if find_registered_secret_exposures(str(value)).applied:
            continue
        result[str(name)] = str(value)
    for name, value in (extra or {}).items():
        if _is_secret_name(name):
            continue
        if find_registered_secret_exposures(str(value)).applied:
            continue
        result[str(name)] = str(value)
    result.setdefault("PATH", "/usr/local/bin:/usr/bin:/bin")
    return result


class RedactingTextIO:
    """Line-buffered text sink that redacts credentials before stderr writes."""

    def __init__(self, stream: Any) -> None:
        self._stream = stream
        self._pending = ""
        self._write_lock = threading.RLock()

    def _write_safe(self, text: str) -> None:
        try:
            safe_text, _report = redact_secrets(text)
        except Exception:
            safe_text = "<redacted:stderr-write>"
        self._stream.write(str(safe_text))

    def write(self, value: Any) -> int:
        text = str(value)
        with self._write_lock:
            self._pending += text
            boundary = self._pending.rfind("\n")
            if boundary >= 0:
                complete = self._pending[:boundary + 1]
                self._pending = self._pending[boundary + 1:]
                self._write_safe(complete)
        return len(text)

    def writelines(self, lines) -> None:
        for line in lines:
            self.write(line)

    def flush(self) -> None:
        with self._write_lock:
            if self._pending:
                self._write_safe(self._pending)
                self._pending = ""
            self._stream.flush()

    def __getattr__(self, name: str):
        return getattr(self._stream, name)


def install_stderr_redaction() -> None:
    """Install the credential-safe stderr boundary once for this process."""
    import sys

    if not isinstance(sys.stderr, RedactingTextIO):
        sys.stderr = RedactingTextIO(sys.stderr)


def _reset_registry_for_tests() -> None:
    """Test isolation only; production code must never unregister secrets."""
    with _LOCK:
        _SECRETS.clear()
