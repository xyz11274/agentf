"""E2BExecutor — e2b SDK direct-connect provider for remote sandbox mode.

Connects to an existing e2b sandbox via ``Sandbox.connect()`` and executes
commands through ``sb.commands.run()``.  This is the most complex provider,
encapsulating:

- **SSL patch** — skip certificate verification for internal path-based routing
- **URL patch** — rewrite envd_api_url from hostname to path-based proxy
- **Path translation** — ``agent_package_dir ↔ /home/user/workspace``
- **Env forwarding** — inject API_KEY/MINIMAX_API_KEY/etc into sandbox
- **Image download** — auto-download generated images from sandbox envd

File operations (read/write/edit/list) run on the **host** filesystem
because the sandbox workspace is mounted to the host.  Paths are
translated bidirectionally via ``_resolve_path()``.

Reference: ADR 20260724-tools-runtime-refactor.md §4.3-4.4 (D-001 decision).
"""

import os
import re
import urllib.parse
import urllib.request

from agentflow_runtime.executors.base import (
    ExecResult,
    FileContent,
    ToolExecutor,
    WriteResult,
)


class E2BExecutor(ToolExecutor):
    """e2b SDK direct-connect provider — remote sandbox mode (D-001 decision).

    The ``endpoint`` is injected by the daemon in the format:
    ``https://host:port/sandbox/{sandbox_id}/{port}``
    """

    # e2b sandbox internal workspace path
    SANDBOX_WORKSPACE = "/home/user/workspace"

    # Environment variables forwarded into the sandbox
    _FORWARD_ENV_KEYS = ("API_KEY", "MINIMAX_API_KEY", "AF_LLM_MODEL", "E2B_API_KEY")

    # Regex to extract image file paths from command stdout
    _IMAGE_PATH_RE = re.compile(
        r"/home/user/workspace/[^\s\"\'\\]+\.(?:png|jpe?g|webp|gif)",
        re.IGNORECASE,
    )

    def __init__(self, endpoint: str):
        """Initialize from a daemon-injected sandbox endpoint URL.

        Args:
            endpoint: URL like ``https://host:port/sandbox/{id}/{port}``.

        Raises:
            ValueError: if sandbox_id cannot be parsed from endpoint.
            RuntimeError: if ``E2B_API_KEY`` environment variable is not set.
        """
        match = re.search(r"/sandbox/([0-9a-f-]+)/", endpoint)
        if not match:
            raise ValueError(f"Cannot parse sandbox_id from endpoint: {endpoint}")
        self.sandbox_id = match.group(1)

        self.api_key = os.environ.get("E2B_API_KEY", "")
        if not self.api_key:
            raise RuntimeError("E2B_API_KEY not set, cannot use E2BExecutor")

        self.agent_package_dir = os.environ.get(
            "AF_AGENT_PACKAGE_DIR", os.getcwd()
        )

    # -- sandbox connection -------------------------------------------------

    def _connect(self, timeout: int = 90):
        """Connect to the e2b sandbox and apply SSL + URL patches.

        Returns a connected ``Sandbox`` instance with path-based routing.
        """
        from agentflow_runtime.common.sandbox_patch import (
            apply_ssl_patch,
            patch_envd_url,
        )
        from e2b_code_interpreter import Sandbox

        apply_ssl_patch()
        sb = Sandbox.connect(self.sandbox_id, timeout=timeout)
        patch_envd_url(sb)
        return sb

    # -- exec_command -------------------------------------------------------

    def exec_command(
        self, command: str, timeout: int = 30, workdir: str | None = None
    ) -> ExecResult:
        """Execute a shell command via e2b SDK ``commands.run``.

        Translates host paths to sandbox paths, forwards env vars, and
        translates output paths back.  Downloads generated images on success.
        """
        sdk_timeout = max(timeout * 2, 90)

        try:
            sb = self._connect(timeout=sdk_timeout)

            # Translate host paths → sandbox paths
            sandbox_cmd = command.replace(
                self.agent_package_dir, self.SANDBOX_WORKSPACE
            )

            # Forward env vars
            exports = ""
            for key in self._FORWARD_ENV_KEYS:
                val = os.environ.get(key, "")
                if val:
                    exports += f"export {key}={val!r}; "
            if exports:
                sandbox_cmd = exports + sandbox_cmd

            execution = sb.commands.run(
                sandbox_cmd,
                cwd=self.SANDBOX_WORKSPACE,
                timeout=sdk_timeout,
            )

            # Translate sandbox paths → host paths in output
            stdout = (execution.stdout or "").replace(
                self.SANDBOX_WORKSPACE, self.agent_package_dir
            )
            stderr = (execution.stderr or "").replace(
                self.SANDBOX_WORKSPACE, self.agent_package_dir
            )
            exit_code = execution.exit_code or 0

            # Auto-download generated images on success
            if exit_code == 0:
                self._download_images(sb, execution.stdout or "")

            return ExecResult(stdout=stdout, stderr=stderr, exit_code=exit_code)

        except Exception as e:
            # SDK exceptions may carry partial execution results
            stdout = getattr(e, "stdout", "") or ""
            stderr = getattr(e, "stderr", "") or ""
            code = getattr(e, "exit_code", None)

            if stdout:
                stdout = stdout.replace(
                    self.SANDBOX_WORKSPACE, self.agent_package_dir
                )
            if stderr:
                stderr = stderr.replace(
                    self.SANDBOX_WORKSPACE, self.agent_package_dir
                )

            return ExecResult(
                stdout=stdout,
                stderr=stderr,
                exit_code=code if code is not None else -1,
                error=f"Sandbox SDK exec failed: {e}",
            )

    # -- image download -----------------------------------------------------

    def _download_images(self, sb, stdout: str) -> None:
        """Extract image file paths from stdout and download to host.

        Best-effort — download failures are silently skipped (images are
        a side-effect, not the primary command output).

        Source: outfit bash.py L118-151.
        """
        for img_path in self._IMAGE_PATH_RE.findall(stdout):
            local_path = img_path.replace(
                self.SANDBOX_WORKSPACE, self.agent_package_dir, 1
            )
            try:
                data = self._fetch_sandbox_file(sb, img_path)
                if data is not None:
                    parent = os.path.dirname(local_path)
                    if parent:
                        os.makedirs(parent, exist_ok=True)
                    with open(local_path, "wb") as f:
                        f.write(data)
            except Exception:
                pass  # best-effort

    @staticmethod
    def _fetch_sandbox_file(sb, path: str) -> bytes | None:
        """Fetch a file's raw bytes from the sandbox envd /files endpoint.

        Falls back to ``sb.files.read()`` if envd URL is unavailable.
        """
        import ssl as _ssl

        envd_url = getattr(sb, "_envd_api_url", None)
        if envd_url:
            ctx = _ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = _ssl.CERT_NONE
            params = urllib.parse.urlencode({"path": path, "username": "user"})
            with urllib.request.urlopen(
                f"{envd_url}/files?{params}", timeout=30, context=ctx
            ) as resp:
                return resp.read()
        else:
            data = sb.files.read(path)
            if isinstance(data, str):
                data = data.encode("latin-1")
            return data

    # -- path translation ---------------------------------------------------

    def _resolve_path(self, path: str) -> str:
        """Translate sandbox paths to host paths.

        - Sandbox paths (``/home/user/workspace/...``) → host ``agent_package_dir/...``
        - Relative paths → resolved against ``agent_package_dir``
        - Absolute host paths → returned as-is
        """
        if path.startswith(self.SANDBOX_WORKSPACE):
            return path.replace(self.SANDBOX_WORKSPACE, self.agent_package_dir)
        if not os.path.isabs(path):
            return os.path.join(self.agent_package_dir, path)
        return path

    def _to_sandbox_path(self, path: str) -> str:
        """Translate host path → sandbox path (reverse of _resolve_path)."""
        if path.startswith(self.SANDBOX_WORKSPACE):
            return path
        if path.startswith(self.agent_package_dir):
            return path.replace(self.agent_package_dir, self.SANDBOX_WORKSPACE)
        if not os.path.isabs(path):
            return os.path.join(self.SANDBOX_WORKSPACE, path)
        return path

    # -- read_file ----------------------------------------------------------

    def read_file(self, path: str, offset: int = 0, limit: int = 0) -> FileContent:
        """Read file from sandbox via SDK commands.run (cat).

        Path is translated to sandbox path and read inside the sandbox,
        then offset/limit slicing is done on the host side.
        """
        sandbox_path = self._to_sandbox_path(path)

        try:
            sb = self._connect()
            execution = sb.commands.run(
                f"cat '{sandbox_path}' 2>/dev/null; printf '\nEXIT_CODE=%s\n' $?",
                cwd=self.SANDBOX_WORKSPACE,
                timeout=30,
            )

            stdout = execution.stdout or ""

            # Parse exit code from the sentinel line
            parts = stdout.rsplit("\nEXIT_CODE=", 1)
            if len(parts) == 2:
                content_part = parts[0]
                sentinel_code = parts[1].strip()
                if sentinel_code and sentinel_code != "0":
                    return FileContent(error=f"File not found: {path}")
            else:
                content_part = stdout

            content_part = content_part.replace(
                self.SANDBOX_WORKSPACE, self.agent_package_dir
            )

            total_lines = len(content_part.splitlines())

            if offset > 0 or limit > 0:
                lines = content_part.splitlines(keepends=True)
                start = max(0, offset - 1) if offset > 0 else 0
                if limit > 0:
                    selected = lines[start : start + limit]
                else:
                    selected = lines[start:]
                content_part = "".join(selected)

            return FileContent(content=content_part, total_lines=total_lines)

        except Exception as e:
            return FileContent(error=f"Failed to read file: {e}")

    # -- write_file ---------------------------------------------------------

    def write_file(self, path: str, content: str) -> WriteResult:
        """Write file to host filesystem (workspace is mounted)."""
        resolved = self._resolve_path(path)

        try:
            parent = os.path.dirname(os.path.abspath(resolved))
            if parent:
                os.makedirs(parent, exist_ok=True)

            with open(resolved, "w", encoding="utf-8") as f:
                f.write(content)

            return WriteResult(
                success=True,
                bytes_written=len(content.encode("utf-8")),
            )
        except Exception as e:
            return WriteResult(error=f"Failed to write file: {e}")

    # -- edit_file ----------------------------------------------------------

    def edit_file(
        self,
        path: str,
        old_str: str,
        new_str: str,
        replace_all: bool = False,
    ) -> ExecResult:
        """Edit file on host filesystem with precise text replacement."""
        resolved = self._resolve_path(path)

        if not os.path.isfile(resolved):
            return ExecResult(exit_code=1, error=f"File not found: {path}")

        try:
            with open(resolved, "r", encoding="utf-8") as f:
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

            with open(resolved, "w", encoding="utf-8") as f:
                f.write(new_content)

            return ExecResult(exit_code=0, stdout=f"Edited {path}")
        except Exception as e:
            return ExecResult(exit_code=1, error=f"Failed to edit file: {e}")

    # -- list_dir -----------------------------------------------------------

    def list_dir(self, path: str, recursive: bool = False) -> list[dict]:
        """List directory on host filesystem."""
        resolved = self._resolve_path(path)

        if not os.path.isdir(resolved):
            return []

        entries: list[dict] = []

        if recursive:
            for root, dirs, files in os.walk(resolved):
                dirs[:] = sorted(dirs)
                for name in sorted(files):
                    full = os.path.join(root, name)
                    entries.append({"name": name, "path": full, "is_dir": False})
                for name in sorted(dirs):
                    full = os.path.join(root, name)
                    entries.append({"name": name, "path": full, "is_dir": True})
        else:
            with os.scandir(resolved) as it:
                for entry in sorted(it, key=lambda e: e.name):
                    entries.append(
                        {
                            "name": entry.name,
                            "path": os.path.join(resolved, entry.name),
                            "is_dir": entry.is_dir(),
                        }
                    )

        return entries

    # -- fetch_url ----------------------------------------------------------

    def fetch_url(self, url: str, timeout: int = 30) -> FileContent:
        """Fetch URL content via host urllib (does not route through sandbox)."""
        try:
            req = urllib.request.Request(
                url,
                headers={
                    "User-Agent": "AgentFlow-E2BExecutor/1.0",
                    "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
                },
            )
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                raw = resp.read()
                content_type = resp.headers.get("Content-Type", "")
                charset = "utf-8"
                if "charset=" in content_type:
                    charset = content_type.split("charset=")[-1].split(";")[0].strip()

                try:
                    text = raw.decode(charset, errors="replace")
                except (LookupError, UnicodeDecodeError):
                    text = raw.decode("utf-8", errors="replace")

                if "text/html" in content_type:
                    text = re.sub(r"<script[^>]*>.*?</script>", "", text, flags=re.DOTALL | re.IGNORECASE)
                    text = re.sub(r"<style[^>]*>.*?</style>", "", text, flags=re.DOTALL | re.IGNORECASE)
                    text = re.sub(r"<[^>]+>", " ", text)
                    text = re.sub(r"\s+", " ", text).strip()

                return FileContent(
                    content=text,
                    total_lines=text.count("\n") + 1 if text else 0,
                )
        except Exception as e:
            return FileContent(error=f"Failed to fetch URL: {e}")
