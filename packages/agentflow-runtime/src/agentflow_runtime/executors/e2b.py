"""E2BExecutor — e2b SDK direct-connect provider for remote sandbox mode.

Connects to an existing e2b sandbox via ``Sandbox.connect()`` and executes
commands through ``sb.commands.run()``.  This is the most complex provider,
encapsulating:

- **SSL patch** — skip certificate verification for internal path-based routing
- **URL patch** — rewrite envd_api_url from hostname to path-based proxy
- **Path translation** — ``agent_package_dir ↔ /home/user/workspace``
- **Env forwarding** — inject API_KEY/MINIMAX_API_KEY/etc into sandbox
- **Image download** — auto-download generated images from sandbox envd

File operations (read/write/edit/list) run **inside the sandbox** (session-isolated):
write_file/edit_file/list_dir 通过 e2b SDK 写入沙箱，read_file 在沙箱内 cat，
均不写回宿主机 agent 包目录（避免跨会话数据残留）。产物由 skill_executor 的
_ensure_artifacts_in_package 通过 download_to_host 拉回宿主机 outputs/{session_id}/
后上传 bridge → artifacts_storage。

Reference: ADR 20260724-tools-runtime-refactor.md §4.3-4.4 (D-001 decision).
"""

import logging
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

    # Regex to extract image/video file paths from command stdout
    # 后缀与 collect 白名单对齐(2026-08-14 review):拉回正则必须 ⊆ 收集白名单,
    # 否则 mkv 等被拉回宿主机却无法被 extract_artifact_paths/_artifact_suffixes
    # 收集 → 孤儿文件。白名单见 skill_executor.py 正则与 bridge _MEDIA_SUFFIXES。
    _IMAGE_PATH_RE = re.compile(
        r"/home/user/workspace/[^\s\"\'\\]+\.(?:png|jpe?g|webp|gif|mp4|mov|webm|mp3|wav)",
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
        # 确保沙箱 workspace 存在——commands.run 的 cwd 直接指它会在镜像布局不同
        # 或会话刚建立时崩（cwd does not exist）。smoke test 之所以先 mkdir 再 cd
        # 就是这个原因。幂等：mkdir -p 已存在不报错。
        sb.commands.run(
            f"mkdir -p {self.SANDBOX_WORKSPACE}", timeout=15
        )
        return sb

    # -- env forwarding -----------------------------------------------------

    def _build_env_source_prefix(self) -> str:
        """Build a ``set -a && source .env && set +a &&`` prefix from the agent .env file.

        The agent ``.env`` uses standard ``KEY=VAL`` syntax (no ``export``).
        ``source`` loads these as shell variables — but **shell variables do
        not propagate to child processes**. ``set -a`` (POSIX allexport)
        makes every assignment during ``source`` automatically ``export``,
        so the subsequent ``python3 script.py`` child sees them in ``os.environ``.

        Why not forward via API ``env_vars`` or an ``export KEY=VAL;`` prefix?
          - API/prefix forwarding repeats every var on every command (stateless)
            and leaks secrets into network requests (violates the updated FR-003).
          - The .env file is already sandbox-local (uploaded with the agent
            package at load time — see sandbox-env-injection.md §2). Loading it
            in-sandbox is zero-network, and secrets stay in the sandbox file only.

        ``.env`` missing → return "" (sandbox keeps its native environment).
        The check is done in-shell (``[ -f ... ] &&``) because the file lives
        sandbox-side and cannot be probed from the host ``os.path``.
        """
        env_path = os.path.join(self.SANDBOX_WORKSPACE, ".env")
        # 用 `{ ... ; } 2>/dev/null;` 包裹源逻辑——.env 不存在时整段静默跳过且
        # 不影响后续命令 exit code（`[ -f ] && ... && ` 链在 .env 缺失时会让
        # 后续真命令被 `&&` 短路掉，pwd/ls 根本不跑，exit=1 stderr 空）。
        # `set +a` 显式关 allexport，恢复沙箱原生 shell 状态。
        return (
            f"{{ {{ [ -f {env_path} ] && set -a && source {env_path} && set +a; }} "
            f"2>/dev/null; }}; "
        )

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

            # Inject env vars from the agent package .env file (sandbox-side).
            # .env 随 agent 包加载时上传到 /home/user/workspace/.env(设计文档 §2 链路)。
            # set -a 让 source 期间所有赋值自动 export,子进程 os.environ 能读到。
            # 宿主机 os.environ 不透传——防 PATH/HOME/USER 等 macOS 路径污染 Linux 容器。
            # .env 不存在 → 前缀的 [ -f ] 兜底跳过 source,沙箱用原生环境。
            env_prefix = self._build_env_source_prefix()
            sandbox_cmd = env_prefix + sandbox_cmd

            # cwd 也要宿主→沙箱翻译——bash.py 缺省 cwd 透传的是宿主 agent_package_dir，
            # 直接给 SDK 会因沙箱内不存在该路径而崩。用 _to_sandbox_path 翻译后再传
            # （宿主 agent_package_dir → /home/user/workspace，恰好等于 SANDBOX_WORKSPACE）；
            # 显式 cwd override（沙箱内绝对路径）走翻译仍不变（_to_sandbox_path 对沙箱路径原样返回）。
            sandbox_cwd = self._to_sandbox_path(workdir) if workdir else self.SANDBOX_WORKSPACE
            execution = sb.commands.run(
                sandbox_cmd,
                cwd=sandbox_cwd,
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

            # F7.1：error 字段补命令上下文——把发送给沙箱的 sandbox_cmd 拼进 error，
            # 让 LLM 能看到"我发了什么→沙箱返回了什么"，从空 error 中能自诊断路径错。
            # 原本只写 `Sandbox SDK exec failed: {e}`，e.__str__ 末尾 stderr 空时 LLM
            # 看到空 error 完全无法判断"命令不存在""路径错""权限错"还是"命令本身逻辑错"。
            # _connect 抛异常时 sandbox_cmd 尚未赋值（try 块 line 132），用 locals().get 兜底
            # 避 UnboundLocalError——_connect 失败的 error 里 cmd="<connect failed>"。
            sandbox_cmd_local = locals().get("sandbox_cmd", "<connect/setup failed before sandbox_cmd assigned>")
            cmd_preview = sandbox_cmd_local[:200] if len(sandbox_cmd_local) > 200 else sandbox_cmd_local
            return ExecResult(
                stdout=stdout,
                stderr=stderr,
                exit_code=code if code is not None else -1,
                error=(
                    f"Sandbox SDK exec failed (cmd={cmd_preview!r}, "
                    f"exit={code}): {e}"
                ),
            )

    # -- image download -----------------------------------------------------

    def _download_images(self, sb, stdout: str) -> None:
        """Extract image/video file paths from stdout and download to host.

        Best-effort — download failures are silently skipped (media are
        a side-effect, not the primary command output).

        2026-08-14 修复:正则从仅图片扩展为图片+视频(mp4/mov/webm/mkv/mp3/wav)。
        此前视频只在沙箱内,宿主机无文件 → collect 阶段 os.path.isfile 失败 →
        skipped_missing → 前端收不到视频产物。与图片同一拉回机制(生成即落盘)。

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
            if isinstance(data, bytearray):
                return bytes(data)
            return data.encode("utf-8") if isinstance(data, str) else None

    def download_to_host(self, sandbox_path: str, local_path: str) -> bool:
        """Download a file from the sandbox to the host filesystem.

        产物收集链路（skill_executor._ensure_artifacts_in_package）在 e2b 模式下
        调用：把沙箱内生成的产物（write_file / bash 生成的文件）拉回宿主机
        outputs/{session_id}/ 后，再上传 bridge → artifacts_storage。

        走 envd /files 端点取原始 bytes（与 _download_images 一致，I-binary-safe），
        禁止 sb.files.read() 的 str 返回（会乱码二进制）。

        Returns:
            True 拉回成功；False 沙箱内无该文件或下载失败（best-effort）。
        """
        try:
            sb = self._connect()
            data = self._fetch_sandbox_file(sb, sandbox_path)
            if data is None:
                logging.warning(
                    "artifact.sandbox_download.missing sandbox_path=%s local=%s",
                    sandbox_path, local_path,
                )
                return False
            parent = os.path.dirname(os.path.abspath(local_path))
            if parent:
                os.makedirs(parent, exist_ok=True)
            with open(local_path, "wb") as f:
                f.write(data)
            logging.info(
                "artifact.sandbox_download.done sandbox_path=%s local=%s bytes=%d",
                sandbox_path, local_path, len(data),
            )
            return True
        except Exception as e:
            logging.warning(
                "artifact.sandbox_download.error sandbox_path=%s local=%s err=%s",
                sandbox_path, local_path, e,
            )
            return False

    # -- path translation ---------------------------------------------------

    def _to_sandbox_path(self, path: str) -> str:
        """Translate host path → sandbox path.

        - 已是沙箱路径（/home/user/workspace/...）→ 原样返回
        - 宿主机 agent_package_dir 前缀 → 替换为沙箱 workspace
        - 相对路径 → 拼到沙箱 workspace 下
        - 其他绝对路径 → 原样返回

        边界安全：agent_package_dir 前缀判断加 os.sep 边界（含 == 与 +os.sep 二分），
        避免 /fake/agent/package-backup/... 这种姊妹目录前缀碰撞被误替换为
        /home/user/workspace-backup/...（2026-08-10 review 发现）。
        """
        if path.startswith(self.SANDBOX_WORKSPACE + os.sep) or path == self.SANDBOX_WORKSPACE:
            return path
        if path == self.agent_package_dir or path.startswith(self.agent_package_dir + os.sep):
            return path.replace(self.agent_package_dir, self.SANDBOX_WORKSPACE, 1)
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
                f"cat '{sandbox_path}' 2>/dev/null; printf '\\nEXIT_CODE=%s\\n' $?",
                cwd=self.SANDBOX_WORKSPACE,
                timeout=30,
            )

            stdout = execution.stdout or ""

            # Parse exit code from the sentinel line
            # sentinel 格式 '\nEXIT_CODE=<code>\n' 的前缀 \n 是 sentinel 起始分隔，
            # 但 sentinel 构造时 printf '\nEXIT_CODE=%s\n' 把 \n 当 sentinel 自己的前缀，
            # 与 content 的行尾 \n 重叠。切片时把 marker 前缀 \n 还给 content——
            # content_part = stdout[:marker_pos + 1]——保留 content 的所有行（含末尾空行），
            # total_lines 与文件实际行数一致。
            # 不用 rsplit：rsplit 把分隔符前缀 \n 吞进分隔段，丢空行
            # （content '{"key": "value"}\n\n' 被截成 '{"key": "value"}\n'，多算一个空行）。
            sentinel_marker = "\nEXIT_CODE="
            marker_pos = stdout.rfind(sentinel_marker)
            if marker_pos != -1:
                sentinel_seg = stdout[marker_pos + len(sentinel_marker):]
                sentinel_code = sentinel_seg.strip()
                if sentinel_code and sentinel_code != "0":
                    return FileContent(error=f"File not found: {path}")
                # marker_pos 是 sentinel 前缀 \n 的位置，+1 还给 content
                content_part = stdout[:marker_pos + 1]
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
        """Write file **inside the sandbox** (session-isolated), not host.

        e2b 模式下文件留在沙箱内，由产物收集链路（_ensure_artifacts_in_package
        → e2b SDK 拉回 → outputs/{session_id}/ → upload storage）收走，避免
        直接写回宿主机 agent 包目录造成跨会话数据残留。返回体带沙箱内 path，
        供 extract_structured_artifacts 识别产物。
        """
        sandbox_path = self._to_sandbox_path(path)
        try:
            sb = self._connect()
            sb.files.write(sandbox_path, content.encode("utf-8"))
            return WriteResult(
                success=True,
                bytes_written=len(content.encode("utf-8")),
                path=sandbox_path,
            )
        except Exception as e:
            return WriteResult(error=f"Failed to write file in sandbox: {e}", path=sandbox_path)

    # -- edit_file ----------------------------------------------------------

    def edit_file(
        self,
        path: str,
        old_str: str,
        new_str: str,
        replace_all: bool = False,
    ) -> ExecResult:
        """Edit file **inside the sandbox** (read-replace-write via e2b SDK).

        与 write_file 一致：文件在沙箱内，不写回宿主机，避免跨会话残留。
        """
        sandbox_path = self._to_sandbox_path(path)

        try:
            sb = self._connect()
            content = sb.files.read(sandbox_path)

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

            sb.files.write(sandbox_path, new_content.encode("utf-8"))

            return ExecResult(exit_code=0, stdout=f"Edited {path}")
        except Exception as e:
            return ExecResult(exit_code=1, error=f"Failed to edit file: {e}")

    # -- list_dir -----------------------------------------------------------

    def list_dir(self, path: str, recursive: bool = False) -> list[dict]:
        """List directory **inside the sandbox** (sb.files.list).

        与 read/write/edit 一致：目录在沙箱内，返回沙箱内路径，不读宿主机。
        """
        sandbox_path = self._to_sandbox_path(path)

        try:
            sb = self._connect()
            # depth=None 表示递归列全量；非递归 depth=1
            entries = sb.files.list(sandbox_path, depth=None if recursive else 1)

            # e2b SDK EntryInfo.type: Optional[FileType]，FileType(Enum) 取值
            # FILE="file"/DIR="dir"。直接用 enum identity 判断——禁止凭 str(Enum)
            # 推断（str(FileType.DIR)=="FileType.DIR" 不.startswith("DIR")，曾导致
            # is_dir 永远 False 的回归 bug，2026-08-10 review 发现）。
            from e2b_code_interpreter import FileType

            result: list[dict] = []
            for entry in entries:
                result.append({
                    "name": entry.name,
                    "path": entry.path,
                    "is_dir": entry.type is FileType.DIR,
                })
            return result
        except Exception:
            return []

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
