"""HTTPExecutor — POST /exec to sandbox_http_server.py provider.

Encapsulates the sandbox-path logic previously inlined in bash.py,
read_file.py, write_file.py, edit_file.py (sandbox_endpoint != None
branches).  All six ToolExecutor operations route through a single
HTTP POST endpoint, mirroring the protocol defined in
contracts/sandbox-http-server.md.

Requires the ``requests`` library (already a project dependency).
"""

import json

import requests

from agentflow_runtime.executors.base import (
    ExecResult,
    FileContent,
    ToolExecutor,
    WriteResult,
)


class HTTPExecutor(ToolExecutor):
    """Sandbox-mode executor: routes all tool ops via HTTP POST.

    The endpoint is injected by the daemon (e.g. ``AF_ARTIFACT_STORAGE_URL``
    or a per-session sandbox URL).  Connection failures are returned as
    error results — the executor never silently falls back to the host
    (B22 invariant).
    """

    def __init__(self, endpoint: str):
        self.endpoint = endpoint.rstrip("/")

    # -- shared transport ---------------------------------------------------

    def _post(
        self,
        tool: str,
        tool_params: dict,
        timeout: int = 30,
        workdir: str | None = None,
    ) -> dict:
        """Send POST /exec to the sandbox server and return parsed JSON.

        Never raises — all transport errors are returned as
        ``{"error": "...", "tool": tool}`` dicts so callers can uniformly
        map them to result dataclasses.
        """
        try:
            resp = requests.post(
                f"{self.endpoint}/exec",
                json={
                    "tool": tool,
                    "tool_params": tool_params,
                    "opts": {
                        "timeout_ms": int(timeout * 1000),
                        "working_dir": workdir or "",
                        "env_vars": {},
                    },
                },
                timeout=timeout + 5,
            )
            return resp.json()
        except (
            requests.exceptions.ConnectionError,
            OSError,
        ) as e:
            return {
                "error": f"Sandbox endpoint unreachable: {self.endpoint} ({e})",
                "tool": tool,
            }
        except (json.JSONDecodeError, ValueError) as e:
            return {
                "error": f"Sandbox returned invalid JSON: {e}",
                "tool": tool,
            }

    # -- exec_command -------------------------------------------------------

    def exec_command(
        self, command: str, timeout: int = 30, workdir: str | None = None
    ) -> ExecResult:
        data = self._post("bash", {"command": command}, timeout, workdir)
        return ExecResult(
            stdout=data.get("stdout", ""),
            stderr=data.get("stderr", ""),
            exit_code=data.get("exit_code", 0),
            error=data.get("error"),
            truncated=data.get("truncated", False),
        )

    # -- read_file ----------------------------------------------------------

    def read_file(self, path: str, offset: int = 0, limit: int = 0) -> FileContent:
        data = self._post("read_file", {"path": path})
        return FileContent(
            content=data.get("stdout", ""),
            error=data.get("error"),
        )

    # -- write_file ---------------------------------------------------------

    def write_file(self, path: str, content: str) -> WriteResult:
        data = self._post(
            "write_file",
            {"path": path, "content": content, "mode": "w"},
        )
        success = data.get("exit_code", 1) == 0 and "error" not in data
        return WriteResult(
            success=success,
            error=data.get("error"),
            bytes_written=len(content.encode("utf-8")) if success else 0,
        )

    # -- edit_file ----------------------------------------------------------

    def edit_file(
        self,
        path: str,
        old_str: str,
        new_str: str,
        replace_all: bool = False,
    ) -> ExecResult:
        data = self._post(
            "edit_file",
            {
                "path": path,
                "old_str": old_str,
                "new_str": new_str,
                "replace_all": replace_all,
            },
        )
        return ExecResult(
            stdout=data.get("stdout", ""),
            stderr=data.get("stderr", ""),
            exit_code=data.get("exit_code", 0),
            error=data.get("error"),
        )

    # -- list_dir -----------------------------------------------------------

    def list_dir(self, path: str, recursive: bool = False) -> list[dict]:
        data = self._post(
            "list_dir",
            {"path": path, "recursive": recursive},
        )
        if "error" in data:
            return []
        stdout = data.get("stdout", "")
        try:
            return json.loads(stdout) if stdout else []
        except (json.JSONDecodeError, TypeError):
            return []

    # -- fetch_url ----------------------------------------------------------

    def fetch_url(self, url: str, timeout: int = 30) -> FileContent:
        data = self._post("web_fetch", {"url": url}, timeout=timeout)
        return FileContent(
            content=data.get("stdout", ""),
            error=data.get("error"),
        )
