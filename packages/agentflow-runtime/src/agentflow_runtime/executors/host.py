"""HostExecutor — subprocess.run + open() provider for sandbox-mode=none.

Encapsulates the host-path logic previously inlined in bash.py, read_file.py,
write_file.py, edit_file.py, web_fetch.py (sandbox_endpoint=None branches).

Uses only Python stdlib (subprocess, os, urllib) — no external dependencies.
"""

import gzip
import os
import re
import subprocess
import urllib.request

from agentflow_runtime.executors.base import (
    ExecResult,
    FileContent,
    ToolExecutor,
    WriteResult,
)
from agentflow_runtime.security import (
    has_registered_secrets,
    safe_subprocess_environment,
)


class _NoRedirectHandler(urllib.request.HTTPRedirectHandler):
    """Keep model-selected fetches from crossing the validated URL boundary."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


_SAFE_URL_OPENER = urllib.request.build_opener(
    urllib.request.ProxyHandler({}),
    _NoRedirectHandler(),
)


class HostExecutor(ToolExecutor):
    """Host-mode executor: runs commands and file ops directly on the host.

    This is the sandbox-mode=none backward-compatible provider (FR-006).
    """

    # -- exec_command -------------------------------------------------------

    def exec_command(self, command: str, timeout: int = 30, workdir: str | None = None) -> ExecResult:
        allow_unsafe = os.environ.get(
            "AF_ALLOW_UNSAFE_LOCAL_SECRET_TOOLS", ""
        ).strip().lower() in {"1", "true", "yes"}
        if not allow_unsafe:
            reason = (
                "Host command blocked while credentials are loaded"
                if has_registered_secrets()
                else "Host command blocked; use an isolated sandbox or explicit unsafe opt-in"
            )
            return ExecResult(error=reason, exit_code=-1)
        # bash.py 缺省 cwd 透传宿主 agent_package_dir（非 None），workdir 永远非空，
        # 故 fallback 分支实际走不进去——保留兜底仅为防御性兜底（应对未来其他调用方传 None）。
        cwd = workdir or os.environ.get("AF_AGENT_PACKAGE_DIR") or os.getcwd()
        try:
            result = subprocess.run(
                command,
                shell=True,
                capture_output=True,
                text=True,
                timeout=timeout,
                cwd=cwd,
                env=safe_subprocess_environment(),
            )
            return ExecResult(
                stdout=result.stdout,
                stderr=result.stderr,
                exit_code=result.returncode,
            )
        except subprocess.TimeoutExpired:
            return ExecResult(
                error=f"Command timed out after {timeout}s",
                exit_code=-1,
            )
        except Exception as e:
            return ExecResult(error=str(e), exit_code=-1)

    # -- read_file ----------------------------------------------------------

    def read_file(self, path: str, offset: int = 0, limit: int = 0) -> FileContent:
        if not os.path.isfile(path):
            return FileContent(error=f"File not found: {path}")

        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                # Text reads must not send binary payloads into model context.
                binary_error = "Binary content detected; read_file only returns text. Use a binary-aware tool to inspect file metadata."
                if "\x00" in f.read(8192):
                    return FileContent(error=binary_error)
                f.seek(0)
                if offset > 0 or limit > 0:
                    lines = f.readlines()
                    start = max(0, offset - 1) if offset > 0 else 0
                    if limit > 0:
                        selected = lines[start : start + limit]
                    else:
                        selected = lines[start:]
                    content = "".join(selected)
                    total_lines = len(lines)
                else:
                    content = f.read()
                    total_lines = len(content.splitlines())

            if "\x00" in content:
                return FileContent(error=binary_error)
            return FileContent(content=content, total_lines=total_lines)
        except PermissionError:
            return FileContent(error=f"Permission denied: {path}")
        except Exception as e:
            return FileContent(error=f"Failed to read file: {e}")

    # -- write_file ---------------------------------------------------------

    def write_file(self, path: str, content: str) -> WriteResult:
        try:
            parent = os.path.dirname(os.path.abspath(path))
            if parent:
                os.makedirs(parent, exist_ok=True)

            with open(path, "w", encoding="utf-8") as f:
                f.write(content)

            return WriteResult(
                success=True,
                bytes_written=len(content.encode("utf-8")),
            )
        except PermissionError:
            return WriteResult(error=f"Permission denied: {path}")
        except IsADirectoryError:
            return WriteResult(error=f"Path is a directory: {path}")
        except Exception as e:
            return WriteResult(error=f"Failed to write file: {e}")

    # -- edit_file ----------------------------------------------------------

    def edit_file(self, path: str, old_str: str, new_str: str, replace_all: bool = False) -> ExecResult:
        if not os.path.isfile(path):
            return ExecResult(exit_code=1, error=f"File not found: {path}")

        try:
            with open(path, "r", encoding="utf-8") as f:
                content = f.read()

            if old_str not in content:
                return ExecResult(
                    exit_code=1,
                    error=f"old_str not found in {path}",
                )

            if not replace_all and content.count(old_str) > 1:
                return ExecResult(
                    exit_code=1,
                    error=f"old_str is not unique in {path} (found {content.count(old_str)} times)",
                )

            if replace_all:
                new_content = content.replace(old_str, new_str)
            else:
                new_content = content.replace(old_str, new_str, 1)

            with open(path, "w", encoding="utf-8") as f:
                f.write(new_content)

            return ExecResult(exit_code=0, stdout=f"Edited {path}")
        except PermissionError:
            return ExecResult(exit_code=1, error=f"Permission denied: {path}")
        except Exception as e:
            return ExecResult(exit_code=1, error=f"Failed to edit file: {e}")

    # -- list_dir -----------------------------------------------------------

    def list_dir(self, path: str, recursive: bool = False) -> list[dict]:
        if not os.path.isdir(path):
            return []

        entries: list[dict] = []

        if recursive:
            for root, dirs, files in os.walk(path):
                dirs[:] = sorted(dirs)
                for name in sorted(files):
                    full = os.path.join(root, name)
                    entries.append({"name": name, "path": full, "is_dir": False})
                for name in sorted(dirs):
                    full = os.path.join(root, name)
                    entries.append({"name": name, "path": full, "is_dir": True})
        else:
            with os.scandir(path) as it:
                for entry in sorted(it, key=lambda e: e.name):
                    entries.append(
                        {
                            "name": entry.name,
                            "path": os.path.join(path, entry.name),
                            "is_dir": entry.is_dir(),
                        }
                    )

        return entries

    # -- fetch_url ----------------------------------------------------------

    def fetch_url(self, url: str, timeout: int = 30) -> FileContent:
        try:
            req = urllib.request.Request(
                url,
                headers={
                    "User-Agent": "AgentFlow-HostExecutor/1.0",
                    "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
                },
            )
            with _SAFE_URL_OPENER.open(req, timeout=timeout) as resp:
                raw = resp.read()
                if "gzip" in (resp.headers.get("Content-Encoding", "") or "").lower():
                    raw = gzip.decompress(raw)
                content_type = resp.headers.get("Content-Type", "")
                charset = "utf-8"
                if "charset=" in content_type:
                    charset = content_type.split("charset=")[-1].split(";")[0].strip()

                try:
                    text = raw.decode(charset, errors="replace")
                except (LookupError, UnicodeDecodeError):
                    text = raw.decode("utf-8", errors="replace")

                # Strip HTML tags for cleaner output
                if "text/html" in content_type:
                    text = re.sub(r"<script[^>]*>.*?</script>", "", text, flags=re.DOTALL | re.IGNORECASE)
                    text = re.sub(r"<style[^>]*>.*?</style>", "", text, flags=re.DOTALL | re.IGNORECASE)
                    text = re.sub(r"<[^>]+>", " ", text)
                    text = re.sub(r"\s+", " ", text).strip()

                return FileContent(content=text, total_lines=text.count("\n") + 1 if text else 0)
        except Exception as e:
            return FileContent(error=f"Failed to fetch URL: {e}")
