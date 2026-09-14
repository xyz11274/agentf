"""Private local HTTP credentials, never placed in a command's workspace/env."""

from __future__ import annotations

import hmac
import os
from pathlib import Path
import re
import secrets
import stat
import tempfile
import urllib.request
from urllib.parse import urlsplit

from agentflow_runtime.security import register_secret


HEADER = "X-AgentFlow-Local-Token"


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}), _NoRedirect())


def local_sandbox_urlopen(request, timeout):
    """Keep local authentication off ambient proxies and redirect targets."""
    return _OPENER.open(request, timeout=timeout)


def _directory() -> Path:
    path = Path("/tmp") / f"agentflow-local-auth-{os.getuid()}"
    path.mkdir(mode=0o700, exist_ok=True)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o700:
        raise OSError("local sandbox authentication directory is not private")
    return path


class LocalSandboxAuth:
    """Lifetime of one listener; the header-file format is also read by curl."""

    def __init__(self, port: int):
        if isinstance(port, bool) or not isinstance(port, int) or not 1 <= port <= 65535:
            raise ValueError("invalid local sandbox listener port")
        self.path = _directory() / str(port)
        self.token = secrets.token_hex(32)
        register_secret("local_sandbox.token", self.token)
        header = f"{HEADER}: {self.token}\n"
        fd, name = tempfile.mkstemp(dir=self.path.parent, prefix=".header-")
        try:
            with os.fdopen(fd, "w") as stream:
                stream.write(header)
            os.replace(name, self.path)
        finally:
            if os.path.exists(name):
                os.unlink(name)

    def accepts(self, value: str) -> bool:
        return (isinstance(value, str) and len(value) == len(self.token)
                and value.isascii() and hmac.compare_digest(value, self.token))

    def close(self) -> None:
        try:
            header = _read_header(self.path)
            if self.accepts(header[HEADER]):
                self.path.unlink()
        except (OSError, ValueError):
            pass


def _read_header(path: Path) -> dict[str, str]:
    fd = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
    with os.fdopen(fd) as stream:
        info = os.fstat(stream.fileno())
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
                or stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1):
            raise OSError("local sandbox authentication file is not private")
        content = stream.read(256)
    match = re.fullmatch(re.escape(HEADER) + r": ([0-9a-f]{64})\n", content)
    if match is None:
        raise ValueError("local sandbox authentication file is invalid")
    token = match.group(1)
    register_secret("local_sandbox.token", token)
    return {HEADER: token}


def local_sandbox_headers(endpoint: str) -> dict[str, str]:
    """Attach only to the local numeric loopback endpoint selected by runtime."""
    if os.environ.get("AF_SANDBOX_MODE") != "local":
        return {}
    parsed = urlsplit(endpoint)
    if (parsed.scheme != "http" or parsed.hostname != "127.0.0.1"
            or not parsed.port or parsed.username or parsed.password
            or parsed.path not in {"", "/"} or parsed.query or parsed.fragment):
        raise ValueError("invalid authenticated local sandbox endpoint")
    return _read_header(_directory() / str(parsed.port))
