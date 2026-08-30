"""Trusted media helpers for credential-bearing structured tools.

Provider credentials stay in the skill executor process.  Only input media and
generated media cross the sandbox boundary; command strings and environment
variables never do.
"""

from __future__ import annotations

import base64
import json
import os
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from contextlib import contextmanager
from pathlib import Path, PurePosixPath
from typing import Iterator
from agentflow_runtime.common.runtime_context import artifact_session_id


from .secrets import find_registered_secret_exposures, register_environment_secrets


IMAGE_SUFFIXES = frozenset({
    ".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp", ".tiff",
})
VIDEO_SUFFIXES = frozenset({".mp4", ".mov", ".webm"})
AUDIO_SUFFIXES = frozenset({".mp3", ".wav"})
MAX_MEDIA_BYTES = 256 * 1024 * 1024
_SANDBOX_PREFIXES = ("/home/user/workspace/", "/workdir/")
_DIRECT_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


class TrustedMediaError(RuntimeError):
    """A safe, user-actionable trusted-media boundary failure."""


def validate_credential_url(url: str) -> str:
    """Allow credential-bearing requests only over HTTPS or loopback HTTP."""
    register_environment_secrets()
    if find_registered_secret_exposures(str(url)).applied:
        raise TrustedMediaError("provider URL contains credential material")
    parsed = urllib.parse.urlsplit(str(url))
    if parsed.username or parsed.password:
        raise TrustedMediaError("provider URL must not contain userinfo")
    if parsed.scheme == "https" and parsed.hostname:
        return str(url)
    if (parsed.scheme == "http"
            and parsed.hostname in {"127.0.0.1", "::1", "localhost"}):
        return str(url)
    raise TrustedMediaError("credential-bearing provider endpoint must use HTTPS")


def direct_urlopen(request_or_url, timeout):
    """Open a provider URL without inheriting ambient proxy credentials paths."""
    return _DIRECT_OPENER.open(request_or_url, timeout=timeout)


def _agent_root() -> Path:
    value = os.environ.get("AF_AGENT_PACKAGE_DIR", "")
    if not value:
        raise TrustedMediaError("AF_AGENT_PACKAGE_DIR is not configured")
    return Path(value).resolve()


def _normalize_workspace_path(path: str) -> PurePosixPath:
    raw = str(path or "").replace("\\", "/")
    if not raw:
        raise TrustedMediaError("media path must not be empty")

    root = _agent_root()
    native = Path(raw)
    if native.is_absolute():
        resolved = native.resolve()
        try:
            raw = resolved.relative_to(root).as_posix()
        except ValueError:
            for prefix in _SANDBOX_PREFIXES:
                if raw.startswith(prefix):
                    raw = raw[len(prefix):]
                    break
            else:
                raise TrustedMediaError("absolute media path is outside the workspace")

    relative = PurePosixPath(raw)
    if relative.is_absolute() or any(part in {"", ".."} for part in relative.parts):
        raise TrustedMediaError("media path escapes the workspace")
    if relative.parts and relative.parts[0].lower() in {
        ".git", "harness", "configs", "config",
    }:
        raise TrustedMediaError("media path targets a protected workspace area")
    return relative


def _validate_suffix(path: PurePosixPath, allowed_suffixes: frozenset[str]) -> None:
    if path.suffix.lower() not in allowed_suffixes:
        raise TrustedMediaError(f"unsupported media type: {path.suffix or '<none>'}")


def _post_json(endpoint: str, route: str, payload: dict, timeout: int = 60) -> dict:
    request = urllib.request.Request(
        endpoint.rstrip("/") + route,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with direct_urlopen(request, timeout=timeout) as response:
            result = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        raise TrustedMediaError(
            f"sandbox media transfer failed with HTTP {exc.code}"
        ) from exc
    except (urllib.error.URLError, OSError, ValueError, json.JSONDecodeError) as exc:
        raise TrustedMediaError(f"sandbox media transfer failed: {exc}") from exc
    if not isinstance(result, dict) or result.get("error"):
        raise TrustedMediaError("sandbox rejected media transfer")
    return result


def _decode_media(content_b64: str) -> bytes:
    try:
        content = base64.b64decode(content_b64, validate=True)
    except (ValueError, TypeError) as exc:
        raise TrustedMediaError("sandbox returned invalid media encoding") from exc
    if len(content) > MAX_MEDIA_BYTES:
        raise TrustedMediaError("media exceeds 256 MiB")
    return content


@contextmanager
def materialize_media(
    source: str,
    sandbox_endpoint: str = "",
    *,
    allowed_suffixes: frozenset[str] = IMAGE_SUFFIXES,
) -> Iterator[str]:
    """Yield a local path or an approved remote/data URL for provider input."""
    value = str(source or "")
    register_environment_secrets()
    if find_registered_secret_exposures(value).applied:
        raise TrustedMediaError("media input contains credential material")
    if value.startswith("data:"):
        if len(value.encode("utf-8")) > MAX_MEDIA_BYTES * 2:
            raise TrustedMediaError("data URL exceeds the media size limit")
        yield value
        return
    if value.startswith(("https://", "http://")):
        parsed = urllib.parse.urlsplit(value)
        if parsed.username is not None or parsed.password is not None:
            raise TrustedMediaError("remote media URL must not contain userinfo")
        if parsed.scheme != "https" and parsed.hostname not in {
            "127.0.0.1", "::1", "localhost",
        }:
            raise TrustedMediaError("remote media URL must use HTTPS")
        yield value
        return

    relative = _normalize_workspace_path(value)
    _validate_suffix(relative, allowed_suffixes)
    local_path = (_agent_root() / Path(*relative.parts)).resolve()
    if local_path.is_file():
        yield str(local_path)
        return
    if not sandbox_endpoint:
        raise TrustedMediaError(f"media file not found: {relative.as_posix()}")

    result = _post_json(
        sandbox_endpoint,
        "/artifact/download",
        {"path": relative.as_posix()},
        timeout=120,
    )
    content = _decode_media(str(result.get("content_b64", "")))
    with tempfile.TemporaryDirectory(prefix="agentflow-media-in-") as temp_dir:
        temp_path = Path(temp_dir) / ("input" + relative.suffix.lower())
        temp_path.write_bytes(content)
        yield str(temp_path)


@contextmanager
def temporary_media_output(requested_path: str) -> Iterator[str]:
    """Yield a private host path with the requested media suffix."""
    relative = _normalize_workspace_path(requested_path)
    _validate_suffix(relative, IMAGE_SUFFIXES | VIDEO_SUFFIXES | AUDIO_SUFFIXES)
    with tempfile.TemporaryDirectory(prefix="agentflow-media-out-") as temp_dir:
        path = Path(temp_dir) / ("result" + relative.suffix.lower())
        yield str(path)


def publish_media(
    local_path: str,
    requested_path: str,
    sandbox_endpoint: str = "",
) -> str:
    """Publish generated bytes without exposing a provider credential."""
    register_environment_secrets()
    relative = _normalize_workspace_path(requested_path)
    _validate_suffix(relative, IMAGE_SUFFIXES | VIDEO_SUFFIXES | AUDIO_SUFFIXES)
    content = Path(local_path).read_bytes()
    if len(content) > MAX_MEDIA_BYTES:
        raise TrustedMediaError("generated media exceeds 256 MiB")
    exposure = find_registered_secret_exposures(content)
    if exposure.applied:
        raise TrustedMediaError("generated media blocked by credential isolation policy")

    if sandbox_endpoint:
        _post_json(
            sandbox_endpoint,
            "/artifact/upload",
            {
                "path": relative.as_posix(),
                "content_b64": base64.b64encode(content).decode("ascii"),
            },
            timeout=180,
        )
    else:
        identity = artifact_session_id()
        if not identity:
            raise TrustedMediaError("artifact session identity is unavailable")
        workspace_prefix = PurePosixPath("workspace", identity)
        if relative.parts[:2] == workspace_prefix.parts:
            published_relative = relative
        else:
            published_relative = workspace_prefix / relative
        destination = (_agent_root() / Path(*published_relative.parts)).resolve()
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(content)
        relative = published_relative
    return relative.as_posix()
