#!/usr/bin/env python3
"""Fail closed when tracked or staged source contains credential material.

The scanner is deliberately provider-neutral.  Its primary signal is the
credential's source name (key/token/secret/password/credential/etc.), not the
shape of one vendor's value.  A small set of well-known prefixes is only a
defence-in-depth fallback for values assigned to an innocuous name.

Findings contain path, line, rule and field name only.  Secret values and
value-derived hashes are never printed.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


_SECRET_NAME_RE = re.compile(
    r"(?:^|[_-])(?:"
    r"api[_-]?key|secret|password|passwd|credentials?|authorization|"
    r"private[_-]?key|access[_-]?key|client[_-]?secret|"
    r"signing[_-]?key|auth[_-]?key|cookie"
    r")(?:$|[_-])",
    re.IGNORECASE,
)
_ASSIGNMENT_RE = re.compile(
    r"(?<![A-Za-z0-9_])"
    r"(?P<name>[\"']?[A-Za-z_][A-Za-z0-9_.-]*[\"']?)"
    r"\s*(?:=|:)\s*"
    r"(?P<value>\"[^\"\n]*\"|'[^'\n]*'|[^\s,;#}\]]+)"
)
_CLI_RE = re.compile(
    r"(?i)--(?P<name>[A-Za-z0-9_-]*(?:key|token|secret|password|credential)[A-Za-z0-9_-]*)"
    r"(?:=|\s+)(?P<value>[^\s]+)"
)
_AUTH_RE = re.compile(
    r"(?i)authorization\s*[:=]\s*(?:bearer|basic)\s+(?P<value>[A-Za-z0-9._~+/=-]{8,})"
)
_URL_USERINFO_RE = re.compile(
    r"(?i)https?://(?P<value>[^\s/@:]+:[^\s/@]+)@"
)
_PEM_RE = re.compile(
    r"-----BEGIN (?:[A-Z0-9 ]+ )?PRIVATE KEY-----"
)
_PREFIX_PATTERNS = (
    re.compile(r"\bsk-[A-Za-z0-9._-]{20,}\b"),
    re.compile(r"\bark-[A-Za-z0-9._-]{20,}\b"),
    re.compile(r"\bhf_[A-Za-z0-9]{20,}\b"),
    re.compile(r"\bgh[pousr]_[A-Za-z0-9]{20,}\b"),
    re.compile(r"\bxox[baprs]-[A-Za-z0-9-]{20,}\b"),
    re.compile(r"\bAKIA[0-9A-Z]{16}\b"),
    re.compile(r"\bAIza[0-9A-Za-z_-]{30,}\b"),
)
_SAFE_EXACT = {
    "", "none", "null", "true", "false", "str", "string", "bytes",
    "password", "changeme", "undefined", "bearer", "basic", "key", "value",
}
_SAFE_MARKERS = (
    "placeholder", "redacted", "your-", "your_", "example", "dummy",
    "fake-", "mock-", "test-", "not-a-real", "not-used", "<credential",
)
_REFERENCE_PREFIXES = (
    "$", "${", "os.environ", "os.getenv", "getenv(", "settings.",
    "config.", "secret_ref", "vault:", "env:",
)
_TEST_PATH_RE = re.compile(r"(?:^|/)(?:tests?|fixtures?)(?:/|$)|(?:^|/)test_[^/]+\.py$")
_CAMEL_SECRET_RE = re.compile(
    r"(?:api|access|refresh|session|bearer|client|private|signing|auth)(?:key|token)$"
    r"|(?:password|passwd|secret|credentials?)$",
    re.IGNORECASE,
)
_CUSTOM_NAMES_DECL_RE = re.compile(
    r"(?im)^\s*AF_SECRET_ENV_NAMES\s*[:=]\s*[\"\x27]?"
    r"(?P<names>[A-Za-z0-9_.-]+(?:\s*,\s*[A-Za-z0-9_.-]+)*)"
)
_TOKEN_NON_SECRET_PREFIXES = frozenset({
    "completion", "count", "input", "max", "min", "output", "parse",
    "prompt", "usage",
})
_TOKEN_SECRET_FOLLOWERS = frozenset({"credential", "secret", "value"})
_CODE_SUFFIXES = frozenset({
    ".bak", ".c", ".cc", ".cpp", ".go", ".h", ".hpp", ".java",
    ".js", ".jsx", ".py", ".pyi", ".rs", ".ts", ".tsx",
})
_NON_SECRET_TRAILING_SEGMENTS = frozenset({
    "configured", "count", "endpoint", "field", "fields", "file", "filepath",
    "files", "host", "len", "length", "name", "names", "path", "paths",
    "suffix", "suffixes", "type", "url",
})
_FALLBACK_LITERAL_PATTERNS = (
    re.compile(
        r"(?i)(?:\bgetenv|\bget|[.]get)\s*[(]\s*[\"'][^\"']+[\"']\s*,\s*"
        r"(?P<quote>[\"'])(?P<value>.*?)(?P=quote)"
    ),
    re.compile(
        r"(?i)\bdefault\s*=\s*(?P<quote>[\"'])(?P<value>.*?)(?P=quote)"
    ),
    re.compile(
        r"(?i)\bor\s+(?P<quote>[\"'])(?P<value>.*?)(?P=quote)"
    ),
)


@dataclass(frozen=True)
class Finding:
    path: str
    line: int
    rule: str
    label: str = ""


def _safe_label(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", value)[:96] or "credential"


def _is_secret_name(name: str, custom_names: set[str]) -> bool:
    normalized = name.strip().strip("\"'")
    lowered = normalized.lower()
    if lowered in {item.lower() for item in custom_names}:
        return True
    parts = [part for part in re.split(r"[_-]+", lowered) if part]
    if parts and parts[-1] in _NON_SECRET_TRAILING_SEGMENTS:
        return False
    if _SECRET_NAME_RE.search(normalized) or _CAMEL_SECRET_RE.search(normalized):
        return True
    if "token" not in parts:
        return False
    token_index = parts.index("token")
    if token_index + 1 < len(parts):
        return parts[token_index + 1] in _TOKEN_SECRET_FOLLOWERS
    if token_index == 0:
        return True
    return parts[token_index - 1] not in _TOKEN_NON_SECRET_PREFIXES


def _literal(raw: str) -> str:
    value = raw.strip().rstrip(",")
    if value in {"\"", "'"}:
        return ""
    if len(value) >= 2 and value[0] == value[-1] and value[0] in {"\"", "'"}:
        value = value[1:-1]
    return value.strip()


def _is_unquoted_code_expression(path: str, raw_value: str) -> bool:
    raw = raw_value.strip()
    if raw[:1] in {"\"", "'"}:
        return False
    suffix = Path(path).suffix.lower()
    if suffix in _CODE_SUFFIXES:
        return True
    if suffix in {".md", ".mdx", ".rst"}:
        return bool(
            re.match(r"(?i)[frbu]{1,2}[\"']", raw)
            or re.search(r"[()[\].{}+]", raw)
        )
    return False


def _code_expression_has_literal_secret(
    line: str,
    candidate: re.Match[str],
    path: str,
    custom_names: set[str],
) -> bool:
    suffix = line[candidate.end("name"):]
    separator = re.search(r"[=:]", suffix)
    if separator is None:
        return False
    rhs = suffix[separator.end():]
    for pattern in _FALLBACK_LITERAL_PATTERNS:
        for literal_match in pattern.finditer(rhs):
            value = literal_match.group("value").strip()
            if not value or _is_secret_name(value, custom_names):
                continue
            if not _is_safe_literal(value, path, semantic_assignment=True):
                return True
    return False


def _is_safe_literal(value: str, path: str, *, semantic_assignment: bool) -> bool:
    normalized = value.strip()
    lowered = normalized.lower()
    if lowered in _SAFE_EXACT:
        return True
    if normalized in {"\"", "'"}:
        return True
    if "…" in normalized or "..." in normalized:
        return True
    if normalized.startswith(_REFERENCE_PREFIXES):
        return True
    if any(marker in lowered for marker in _SAFE_MARKERS):
        return True
    if re.fullmatch(r"<[^>]+>", normalized):
        return True
    if re.search(r"\$(?:\{|[A-Za-z_])", normalized):
        return True
    if re.fullmatch(r"\$\{[A-Za-z_][A-Za-z0-9_]*(?::-[^}]*)?\}", normalized):
        return True
    if semantic_assignment and _TEST_PATH_RE.search(path):
        # Unit tests frequently use arbitrary sentinel strings.  Known provider
        # prefixes, auth headers, URL userinfo and PEM blocks remain scanned.
        return True
    return False


def scan_bytes(
    path: str,
    content: bytes,
    *,
    custom_names: Iterable[str] = (),
) -> list[Finding]:
    """Scan one file without returning any matched value."""
    if b"\x00" in content:
        return []
    text = content.decode("utf-8", errors="replace")
    custom = {str(name).strip() for name in custom_names if str(name).strip()}
    findings: set[Finding] = set()

    for line_no, line in enumerate(text.splitlines(), 1):
        if _PEM_RE.search(line):
            findings.add(Finding(path, line_no, "private_key_block", "private_key"))

        for match in _PREFIX_PATTERNS:
            for candidate in match.finditer(line):
                value = candidate.group(0)
                if not _is_safe_literal(value, path, semantic_assignment=False):
                    findings.add(Finding(path, line_no, "credential_prefix", "provider_key"))

        for candidate in _AUTH_RE.finditer(line):
            value = candidate.group("value")
            if not _is_safe_literal(value, path, semantic_assignment=False):
                findings.add(Finding(path, line_no, "authorization_literal", "authorization"))

        for candidate in _URL_USERINFO_RE.finditer(line):
            value = candidate.group("value")
            if not _is_safe_literal(value, path, semantic_assignment=False):
                findings.add(Finding(path, line_no, "url_userinfo", "url_credentials"))

        for candidate in _CLI_RE.finditer(line):
            name = candidate.group("name")
            if not _is_secret_name(name, custom):
                continue
            raw_value = candidate.group("value")
            if _is_unquoted_code_expression(path, raw_value):
                continue
            value = _literal(raw_value)
            if not _is_safe_literal(value, path, semantic_assignment=True):
                findings.add(Finding(path, line_no, "credential_cli_argument", _safe_label(name)))

        for candidate in _ASSIGNMENT_RE.finditer(line):
            name = candidate.group("name").strip("\"'")
            if not _is_secret_name(name, custom):
                continue
            raw_value = candidate.group("value")
            if raw_value.strip() == name:
                continue
            if _is_unquoted_code_expression(path, raw_value):
                if _code_expression_has_literal_secret(
                    line, candidate, path, custom,
                ):
                    findings.add(Finding(
                        path, line_no, "credential_assignment", _safe_label(name),
                    ))
                continue
            value = _literal(raw_value)
            if _is_safe_literal(value, path, semantic_assignment=True):
                continue
            findings.add(Finding(path, line_no, "credential_assignment", _safe_label(name)))

    return sorted(findings, key=lambda item: (item.path, item.line, item.rule, item.label))


def _run_git(root: Path, *args: str) -> bytes:
    completed = subprocess.run(
        ["git", *args],
        cwd=root,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode:
        raise RuntimeError("git command failed while collecting scan targets")
    return completed.stdout


def _repo_root() -> Path:
    output = _run_git(Path.cwd(), "rev-parse", "--show-toplevel")
    return Path(output.decode("utf-8", errors="strict").strip())


def _tracked_targets(root: Path) -> list[tuple[str, bytes]]:
    names = _run_git(root, "ls-files", "-z").split(b"\x00")
    targets = []
    for raw_name in names:
        if not raw_name:
            continue
        name = raw_name.decode("utf-8", errors="surrogateescape")
        file_path = root / name
        if not file_path.is_file():
            continue
        try:
            targets.append((name, file_path.read_bytes()))
        except OSError as exc:
            raise RuntimeError(f"could not read tracked path: {name}") from exc
    return targets


def _staged_targets(root: Path) -> list[tuple[str, bytes]]:
    names = _run_git(
        root,
        "diff",
        "--cached",
        "--name-only",
        "--diff-filter=ACMR",
        "-z",
    ).split(b"\x00")
    targets = []
    for raw_name in names:
        if not raw_name:
            continue
        name = raw_name.decode("utf-8", errors="surrogateescape")
        content = _run_git(root, "show", f":{name}")
        targets.append((name, content))
    return targets


def _path_targets(root: Path, paths: Iterable[str]) -> list[tuple[str, bytes]]:
    targets = []
    for value in paths:
        file_path = Path(value)
        if not file_path.is_absolute():
            file_path = root / file_path
        if not file_path.is_file():
            continue
        try:
            name = file_path.resolve().relative_to(root.resolve()).as_posix()
        except ValueError:
            name = file_path.name
        targets.append((name, file_path.read_bytes()))
    return targets


def _declared_custom_names(
    targets: Iterable[tuple[str, bytes]],
) -> set[str]:
    """Collect nonstandard credential variable names declared by the repo."""
    names: set[str] = set()
    for _path, content in targets:
        if b"\x00" in content:
            continue
        text = content.decode("utf-8", errors="replace")
        for match in _CUSTOM_NAMES_DECL_RE.finditer(text):
            names.update(
                item.strip() for item in match.group("names").split(",")
                if item.strip()
            )
    return names


def format_findings(findings: Iterable[Finding]) -> str:
    """Render metadata only; values and value-derived hashes are forbidden."""
    lines = []
    for item in findings:
        suffix = f" field={item.label}" if item.label else ""
        lines.append(f"{item.path}:{item.line}: {item.rule}{suffix}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Scan source files for hard-coded credentials")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--tracked", action="store_true", help="scan all tracked worktree files")
    mode.add_argument("--staged", action="store_true", help="scan the exact staged blobs")
    parser.add_argument("paths", nargs="*", help="specific paths (overrides the default tracked mode)")
    args = parser.parse_args(argv)

    try:
        root = _repo_root()
        if args.paths:
            targets = _path_targets(root, args.paths)
        elif args.staged:
            targets = _staged_targets(root)
        else:
            targets = _tracked_targets(root)
    except RuntimeError as exc:
        print(f"secret scan setup failed: {exc}", file=sys.stderr)
        return 2

    custom_names = {
        value.strip()
        for value in os.environ.get("AF_SECRET_ENV_NAMES", "").split(",")
        if value.strip()
    }
    custom_names.update(_declared_custom_names(targets))
    findings = []
    for path, content in targets:
        findings.extend(scan_bytes(path, content, custom_names=custom_names))

    unique = sorted(set(findings), key=lambda item: (item.path, item.line, item.rule, item.label))
    if unique:
        print(f"secret scan failed: {len(unique)} finding(s)", file=sys.stderr)
        print(format_findings(unique), file=sys.stderr)
        return 1
    print(f"secret scan passed: {len(targets)} file(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
