"""HTTPExecutor — POST /exec to sandbox_http_server.py provider.

Encapsulates the sandbox-path logic previously inlined in bash.py,
read_file.py, write_file.py, edit_file.py (sandbox_endpoint != None
branches).  All six ToolExecutor operations route through a single
HTTP POST endpoint, mirroring the protocol defined in
contracts/sandbox-http-server.md.

Requires the ``requests`` library (already a project dependency).
"""

import json
import os

import requests

from agentflow_runtime.common.runtime_context import artifact_routing_id
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

    @staticmethod
    def _is_within(path: str, root: str) -> bool:
        """Return whether *path* is inside *root* after realpath resolution."""
        try:
            return os.path.commonpath((path, root)) == root
        except ValueError:
            return False

    def _local_path_scope(self, tool: str, path: str) -> tuple[str, str] | None:
        """Resolve one Local file request to its narrowest trusted host root.

        LocalSandbox shares the host filesystem, but its HTTP server starts in
        an isolated temporary cwd. Only the current user's workspace is
        writable; the agent's static skill tree is read-only. Returning no
        scope leaves the server's temporary cwd boundary in force, so paths to
        another user or another package area are still rejected server-side.
        """
        if os.environ.get("AF_SANDBOX_MODE", "") != "local" or not path:
            return None

        agent_pkg = os.environ.get("AF_AGENT_PACKAGE_DIR", "")
        user_id = os.environ.get("AF_USER_ID", "")
        client_id = os.environ.get("AF_CLIENT_ID", "")
        if not agent_pkg or not user_id or not client_id:
            return None

        session_dir = artifact_routing_id(user_id, client_id)
        if os.path.basename(session_dir) != session_dir or session_dir in (".", ".."):
            return None

        package_root = os.path.realpath(agent_pkg)
        workspace_root = os.path.realpath(os.path.join(package_root, "workspace"))
        user_root = os.path.realpath(os.path.join(workspace_root, session_dir))
        if user_root == workspace_root or not self._is_within(user_root, workspace_root):
            return None

        normalized = os.path.normpath(path)
        if os.path.isabs(path):
            resolved = os.path.realpath(path)
        elif normalized == "harness/skills" or normalized.startswith("harness/skills/"):
            resolved = os.path.realpath(os.path.join(package_root, normalized))
        else:
            resolved = os.path.realpath(os.path.join(user_root, normalized))

        if self._is_within(resolved, user_root):
            return resolved, user_root

        skills_root = os.path.realpath(os.path.join(package_root, "harness", "skills"))
        if tool in {"read_file", "list_dir"} and self._is_within(resolved, skills_root):
            return resolved, skills_root
        return None

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
        local_scope = self._local_path_scope(tool, str(tool_params.get("path", "")))
        if local_scope is not None:
            resolved_path, workdir = local_scope
            tool_params = {**tool_params, "path": resolved_path}

        # cwd 按沙箱类型分流：bash.py 缺省 cwd 透传的是宿主 agent_package_dir，
        # 该路径在容器内不存在，透传给 sandbox server 的 subprocess 会 FileNotFoundError 崩。
        # HTTPExecutor 看不到 daemon 的宿主↔容器挂载点映射，但容器内 SANDBOX_WORKDIR=/workdir
        # 由 daemon 注入（Dockerfile L36 + sandbox_docker.cpp L115）——把宿主路径替换为空串，
        # 让 server 端 fallback 链 opts.working_dir → SANDBOX_WORKDIR 落到容器内正确路径。
        # 显式 cwd override（沙箱内绝对路径）不是宿主 agent_package_dir 起始，不受影响。
        # 注：本段 cwd 清洗只对 bash 生效；Local 文件工具使用上方收敛后的用户/skill
        # 工作根，不进入宿主 cwd 清洗。
        if tool == "bash" and workdir:
            agent_pkg_dir = os.environ.get("AF_AGENT_PACKAGE_DIR", "")
            if agent_pkg_dir and workdir.startswith(agent_pkg_dir):
                workdir = ""
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
            data = resp.json()
            if not isinstance(data, dict):
                return {
                    "error": (
                        "Sandbox returned invalid JSON object: "
                        f"expected object, got {type(data).__name__}"
                    ),
                    "tool": tool,
                }

            # /exec 对 HTTP 失败和工具失败都返回 ExecResult JSON。并发过载时
            # 响应为 HTTP 503 + exit_code=1 + stderr，但没有 error 字段；这里
            # 必须补齐错误，否则 read_file 会变成空内容成功，write_file 会变成
            # ``success=false, error=null``。
            raw_error = data.get("error")
            if isinstance(raw_error, dict):
                error_text = (
                    raw_error.get("message")
                    or raw_error.get("code")
                    or json.dumps(raw_error, ensure_ascii=False)
                )
            else:
                error_text = str(raw_error) if raw_error else ""

            status_code = resp.status_code
            exit_code = data.get("exit_code")
            failed = status_code >= 400 or (
                exit_code is not None and exit_code != 0
            )
            if failed:
                detail = error_text or str(data.get("stderr") or "").strip()
                if not detail:
                    detail = f"exit_code={exit_code}"
                data["error"] = (
                    f"Sandbox tool {tool} failed (HTTP {status_code}): {detail}"
                )
            return data
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
        local_scope = self._local_path_scope("write_file", path)
        result_path = local_scope[0] if local_scope is not None else path
        return WriteResult(
            success=success,
            error=data.get("error"),
            bytes_written=len(content.encode("utf-8")) if success else 0,
            path=result_path if success else "",
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
