#!/usr/bin/env python3
"""AgentFoundry 沙箱内 HTTP server。

统一委托 ``agentflow_runtime.harness.tools.*`` 执行工具，消除内联双轨。

端点 POST /exec，按 tool 字段分发到 agentflow_runtime 的 tool 实现。
保留安全边界：危险命令过滤、路径越界检查在委托前完成。

启动方式：
- LocalSandbox（进程级）：宿主机侧绝对路径直接 Popen，--port 0 自动分配
- DockerSandbox/E2BSandbox（容器级）：镜像内 /opt/sandbox/http_server/，固定端口
"""

import argparse
import base64
import concurrent.futures
import fcntl
import importlib
import json
import logging
import os
import pty as pty_mod
import re
import signal
import socket
import struct
import sys
import termios
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

# 默认输出截断阈值（0=不截断，由 daemon 通过 opts 注入实际值）
DEFAULT_OUTPUT_MAX_BYTES = 0

# 并发执行上限
MAX_CONCURRENT = int(os.environ.get("SANDBOX_MAX_CONCURRENT", "8"))
_concurrency_semaphore = threading.Semaphore(MAX_CONCURRENT)

# ── 交互式 PTY 会话表（webshell v0.4，见 L2 §webshell 交互式 PTY）──
# session_id -> {"master_fd": int, "child_pid": int, "rows": int, "cols": int,
#                "output": bytearray, "read_offset": int, "exit_code": int|None}
_pty_sessions: dict[str, dict] = {}
_pty_sessions_lock = threading.Lock()

# 错误码常量（与 contracts/sandbox-http-server.md 对齐）
PATH_NOT_IN_UPLOAD_DIR = "PATH_NOT_IN_UPLOAD_DIR"
_SECURITY_PROFILE = "isolated"
_CREDENTIAL_FILES = {
    ".env", ".mcp.json", ".netrc", ".npmrc", ".pypirc",
    "id_rsa", "id_dsa", "id_ecdsa", "id_ed25519",
}
_CREDENTIAL_SUFFIXES = {".pem", ".key", ".p12", ".pfx", ".jks", ".keystore"}
_LOCAL_SAFE_ENV = {
    "PATH", "PYTHONPATH", "VIRTUAL_ENV", "CONDA_PREFIX", "CONDA_DEFAULT_ENV",
    "LANG", "LC_ALL", "LC_CTYPE", "TZ", "TERM", "TMPDIR", "TEMP", "TMP",
    "HOME", "USER", "LOGNAME", "SHELL", "LD_LIBRARY_PATH",
    "SSL_CERT_FILE", "SSL_CERT_DIR", "REQUESTS_CA_BUNDLE", "CURL_CA_BUNDLE",
    "NO_PROXY", "no_proxy", "SANDBOX_MAX_CONCURRENT",
}


def _restricted_local() -> bool:
    return _SECURITY_PROFILE == "restricted-local"

# 危险命令前缀（宿主侧第一道防线，B2）
DANGEROUS_PREFIXES = (
    "rm -rf /", "sudo ", "sudo\t", ":(){", "fork bomb",
    "mkfs.", "dd if=/dev/zero of=/dev/sd", "shutdown", "reboot",
)

# ``/exec`` is also reachable by model-selected tools.  Caller-supplied
# environment variables therefore never cross this generic tool boundary;
# trusted providers use their host-side adapters instead.


def is_dangerous(command: str) -> bool:
    """检查命令是否含危险前缀（B2）。"""
    stripped = command.strip()
    return any(stripped.startswith(p) or p in stripped for p in DANGEROUS_PREFIXES)


def resolve_safe_path(path: str, allowed_prefix: str) -> str:
    """路径双检（B4）：string prefix + realpath，拒绝越界。"""
    if not path:
        raise ValueError("path must not be empty")
    full = Path(allowed_prefix) / path if not os.path.isabs(path) else Path(path)
    resolved = str(full.resolve())
    allowed = str(Path(allowed_prefix).resolve())
    if not resolved.startswith(allowed + "/") and resolved != allowed:
        raise PathEscapeException(path, allowed)
    return resolved


class PathEscapeException(Exception):
    """路径越界异常（B4）。"""
    def __init__(self, path: str, allowed_prefix: str):
        super().__init__(f"Path escape rejected: '{path}' escapes sandbox workdir '{allowed_prefix}'")
        self.path = path
        self.allowed_prefix = allowed_prefix


def _is_protected_path(path: str, *, for_write: bool = False) -> bool:
    normalized = path.replace("\\", "/").rstrip("/")
    basename = normalized.rsplit("/", 1)[-1].lower()
    if (basename in _CREDENTIAL_FILES or basename.startswith(".env.")
            or Path(basename).suffix.lower() in _CREDENTIAL_SUFFIXES):
        return True
    if not for_write:
        return False
    lowered = f"/{normalized.lower().lstrip('./')}"
    return (
        basename in {"agent.md", "manifest.json", "manifest.jsonc"}
        or "/harness/tools/" in lowered
        or "/harness/hooks/" in lowered
        or ("/harness/skills/" in lowered and "/scripts/" in lowered)
    )


def _scrub_restricted_local_environment() -> None:
    for name in list(os.environ):
        if name not in _LOCAL_SAFE_ENV:
            os.environ.pop(name, None)


def validate_upload_path(path: str) -> str | None:
    """校验上传路径是否为合法的会话级路径。

    接受格式：``workspace/{user_id}_{client_id}/upload/{filename}``
    拒绝：
      - ``workspace/upload/...``（无会话目录，旧格式）
      - ``workspace//upload/...``（空会话目录）
      - 非 ``workspace/`` 前缀的路径
      - 宿主机绝对路径

    返回 ``None`` 表示合法，否则返回 PATH_NOT_IN_UPLOAD_DIR 错误信息。
    """
    # 必须是 workspace/ 开头
    if not path.startswith("workspace/"):
        return (
            f"{PATH_NOT_IN_UPLOAD_DIR}: path must start with 'workspace/{{session}}/upload/', "
            f"got: {path}"
        )
    # 去掉 workspace/ 前缀后，剩余部分格式：{session}/upload/{filename}
    rest = path[len("workspace/"):]
    # 必须包含 /upload/ 且 session 部分不能为空
    upload_idx = rest.find("/upload/")
    if upload_idx <= 0:  # session 为空(=0)或没有 /upload/(=-1)
        return (
            f"{PATH_NOT_IN_UPLOAD_DIR}: path must contain a non-empty session directory "
            f"before '/upload/', got: {path}"
        )
    return None


def _truncate(content: str, max_bytes: int) -> tuple[str, bool]:
    """截断输出（B23）。"""
    if max_bytes <= 0 or len(content.encode("utf-8")) <= max_bytes:
        return content, False
    encoded = content.encode("utf-8")[:max_bytes]
    decoded = encoded.decode("utf-8", errors="ignore")
    return decoded, True


def _exec_result(exit_code: int, stdout: str = "", stderr: str = "",
                 truncated: bool = False, duration_ms: int = 0) -> dict:
    """构造 ExecResult JSON。"""
    return {
        "exit_code": exit_code,
        "stdout": stdout,
        "stderr": stderr,
        "truncated": truncated,
        "duration_ms": duration_ms,
    }


def _exec_tool(tool_name: str, tool_params: dict, opts: dict) -> dict:
    """统一工具执行：委托给 ``agentflow_runtime.harness.tools.{tool_name}``。

    安全层（前置于委托）：
    - ``tool=bash`` → 先检查 ``is_dangerous()``
    - ``tool=read_file/write_file/edit_file/list_dir`` → 先检查 ``resolve_safe_path()``
    """
    workdir = opts.get("working_dir") or os.environ.get("AF_SANDBOX_WORKSPACE") or os.getcwd()
    output_max_bytes = opts.get("output_max_bytes", DEFAULT_OUTPUT_MAX_BYTES)
    resolved_path = ""

    if opts.get("env_vars"):
        return _exec_result(
            1,
            stderr="security_policy_blocked: caller-supplied env_vars are disabled",
        )

    # -- 参数名映射：sandbox_http_server 旧参数名 → runtime tool 参数名 --
    _PARAM_MAP = {
        ("read_file", "path"): "file_path",
        ("write_file", "path"): "file_path",
        ("edit_file", "path"): "file_path",
        ("load_skill", "name"): "skill_name",
    }

    # -- 安全前检（前置层，不等委托） --
    if tool_name == "bash":
        command = tool_params.get("command", "")
        if not command:
            return _exec_result(1, stderr="command must not be empty")
        if is_dangerous(command):
            return _exec_result(1, stderr=f"Dangerous command rejected: {command}")
        if _restricted_local():
            from agentflow_runtime.security.local_command import run_local_command

            # 宿主模式仅在独立受限子进程执行，不经过 HostExecutor 或 unsafe opt-in。
            started = time.monotonic()
            result = run_local_command(command, workdir, opts.get("timeout_ms", 30000) / 1000)
            stdout, out_truncated = _truncate(result["stdout"], output_max_bytes)
            stderr, err_truncated = _truncate(result["stderr"], output_max_bytes)
            return _exec_result(
                result["exit_code"], stdout, stderr,
                result["truncated"] or out_truncated or err_truncated,
                int((time.monotonic() - started) * 1000),
            )

    elif tool_name in ("read_file", "write_file", "edit_file", "list_dir"):
        old_path = tool_params.get("path", "")
        try:
            resolved_path = resolve_safe_path(old_path, workdir)
            if _is_protected_path(
                    resolved_path, for_write=tool_name in {"write_file", "edit_file"}):
                return _exec_result(
                    1, stderr="security_policy_blocked: protected path access denied")
        except (PathEscapeException, ValueError) as e:
            return _exec_result(1, stderr=str(e))

    elif tool_name == "grep":
        pattern = tool_params.get("pattern", "")
        try:
            re.compile(pattern)
        except re.error as e:
            return _exec_result(1, stderr=f"Invalid regex: {e}")

    if tool_name == "list_dir":
        from agentflow_runtime.executors.host import HostExecutor

        entries = HostExecutor().list_dir(
            resolved_path, recursive=bool(tool_params.get("recursive", False))
        )
        stdout, truncated = _truncate(
            json.dumps(entries, ensure_ascii=False), output_max_bytes
        )
        return _exec_result(0, stdout=stdout, truncated=truncated)

    # -- 构建 tool_input（翻译参数名 + 合并 opts 中的相关字段） --
    tool_input = {}
    for k, v in tool_params.items():
        mapped = _PARAM_MAP.get((tool_name, k), k)
        tool_input[mapped] = v
    if "timeout_ms" in opts:
        tool_input["timeout"] = opts["timeout_ms"] // 1000
    if resolved_path:
        tool_input["file_path"] = resolved_path

    # -- cwd override（webshell 跨命令 cwd 延续）：opts.working_dir 优先于默认 agent package dir --
    # 仅对 bash 生效——read_file/write_file/edit_file 走 resolve_safe_path 基准 workdir（上面已取）
    if tool_name == "bash" and opts.get("working_dir"):
        tool_input["cwd"] = opts["working_dir"]

    # -- 委托执行 --
    start = time.monotonic()
    try:
        # 注：list_skills / load_skill 依赖 AF_AGENT_PACKAGE_DIR 找 harness/skills/
        if tool_name in ("list_skills", "load_skill"):
            _old_agent_dir = os.environ.get("AF_AGENT_PACKAGE_DIR")
            os.environ["AF_AGENT_PACKAGE_DIR"] = workdir
        try:
            mod = importlib.import_module(f"agentflow_runtime.harness.tools.{tool_name}")
            if not hasattr(mod, "execute"):
                return _exec_result(1, stderr=f"Tool {tool_name} has no execute() function")
            # 超时执行：用 opts.timeout_ms 作为 HTTP server 层面超时兜底（F2），
            # 同时传给 runtime tool 作为内部超时。
            # 文件路径已经过上方 workdir 边界检查；内部标记避免 runtime wrapper
            # 再按 HTTP server 自身 cwd 误拒绝合法路径。
            execute_args = (tool_input, "sandbox-internal") if resolved_path else (tool_input,)
            timeout_secs = tool_input.get("timeout", 0)
            if timeout_secs and timeout_secs > 0:
                with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                    future = pool.submit(mod.execute, *execute_args)
                    try:
                        result_json = future.result(timeout=timeout_secs)
                    except concurrent.futures.TimeoutError:
                        duration_ms = int((time.monotonic() - start) * 1000)
                        return _exec_result(
                            1,
                            stderr=f"Tool {tool_name} timed out after {timeout_secs}s",
                            duration_ms=duration_ms,
                        )
            else:
                result_json = mod.execute(*execute_args)
            result = json.loads(result_json)
        finally:
            if tool_name in ("list_skills", "load_skill"):
                if _old_agent_dir:
                    os.environ["AF_AGENT_PACKAGE_DIR"] = _old_agent_dir
                else:
                    os.environ.pop("AF_AGENT_PACKAGE_DIR", None)
    except ImportError as e:
        return _exec_result(1, stderr=f"Tool module not found: {tool_name} ({e})")
    except json.JSONDecodeError as e:
        return _exec_result(1, stderr=f"Invalid JSON returned by tool {tool_name}: {e}")
    except Exception as e:
        return _exec_result(1, stderr=f"Tool {tool_name} failed: {type(e).__name__}: {e}")

    duration_ms = int((time.monotonic() - start) * 1000)

    # -- 响应归一化：runtime tool 返回格式 → ExecResult 格式 --
    error = result.get("error") or result.get("stderr", "") or ""
    exit_code = 1 if error else result.get("exit_code", 0)

    if tool_name == "bash":
        stdout = result.get("stdout") or ""
        stderr = result.get("stderr") or result.get("error", "") or ""
        truncated = result.get("truncated", False)
    elif tool_name in ("read_file", "load_skill", "web_fetch"):
        # runtime 返回 {"content": "...", "error": null}
        stdout = result.get("content") or ""
        stderr = error
        truncated = False
    elif tool_name in ("write_file", "edit_file"):
        # runtime 返回 {"success": true, "error": null, "bytes_written": N}
        succeeded = result.get("success")
        if succeeded is None:
            succeeded = not error and exit_code == 0
        stdout = "" if succeeded else error
        stderr = "" if succeeded else error
        exit_code = 0 if succeeded else 1
        truncated = False
    elif tool_name == "list_skills":
        skills = result.get("skills", [])
        stdout = json.dumps(skills, ensure_ascii=False)
        stderr = error if not skills else ""
        exit_code = 0
        truncated = False
    else:
        stdout = result.get("stdout") or ""
        stderr = result.get("stderr") or result.get("error", "") or ""
        truncated = result.get("truncated", False)

    # -- 后处理：截断输出 --
    stdout, stdout_trunc = _truncate(stdout, output_max_bytes)
    stderr, stderr_trunc = _truncate(stderr, output_max_bytes)

    return _exec_result(
        exit_code=exit_code,
        stdout=stdout,
        stderr=stderr,
        truncated=stdout_trunc or stderr_trunc or truncated,
        duration_ms=duration_ms,
    )


# tool 分发表（list_dir 复用 HostExecutor，其余工具委托 runtime tool）
TOOL_DISPATCH = {
    "bash": _exec_tool,
    "read_file": _exec_tool,
    "write_file": _exec_tool,
    "edit_file": _exec_tool,
    "list_dir": _exec_tool,
    "web_fetch": _exec_tool,
    "list_skills": _exec_tool,
    "load_skill": _exec_tool,
    "grep": _exec_tool,
    "glob": _exec_tool,
}


# ── 交互式 PTY：后台读取线程与窗口尺寸 ioctl ─────────────────────────

def _pty_set_winsize(master_fd: int, rows: int, cols: int) -> None:
    """设置 PTY 窗口尺寸（TIOCSWINSZ）。失败静默——非关键路径。"""
    try:
        fcntl.ioctl(
            master_fd,
            termios.TIOCSWINSZ,
            struct.pack("HHHH", rows, cols, 0, 0),
        )
    except OSError as exc:
        logging.warning("sandbox_http_server.pty_set_winsize failed: %s", exc)


def _pty_reader(session_id: str) -> None:
    """后台读 PTY 输出：持续读 master_fd 追加到会话缓冲，进程退出后置 exit_code。

    会话表条目：{"master_fd", "child_pid", "rows", "cols", "output", "read_offset", "exit_code"}。
    EOF / OSError（EIO，PTY 子进程退出）视为进程结束：waitpid 回收并记录退出码。
    """
    with _pty_sessions_lock:
        session = _pty_sessions.get(session_id)
    if session is None:
        return
    master_fd = session["master_fd"]
    try:
        while True:
            try:
                chunk = os.read(master_fd, 4096)
            except OSError:
                break  # EIO：PTY 对端（子进程）已关闭
            if not chunk:
                break
            with _pty_sessions_lock:
                session["output"].extend(chunk)
    finally:
        exit_code: int | None = None
        try:
            _, status = os.waitpid(session["child_pid"], 0)
            if os.WIFEXITED(status):
                exit_code = os.WEXITSTATUS(status)
            elif os.WIFSIGNALED(status):
                exit_code = 128 + os.WTERMSIG(status)
        except (ChildProcessError, OSError):
            exit_code = -1
        with _pty_sessions_lock:
            session["exit_code"] = exit_code
        try:
            os.close(master_fd)
        except OSError:
            pass
        logging.info("sandbox_http_server.pty_reader_end session_id=%s exit_code=%s", session_id, exit_code)


class SandboxHttpHandler(BaseHTTPRequestHandler):
    """沙箱内 HTTP server handler：工具执行端点 POST /exec。"""

    def do_GET(self):
        """不处理 GET。"""
        self._send_json(404, {"error": {
            "code": "NOT_FOUND",
            "message": f"Not found: {self.path}",
        }})

    def do_POST(self):
        """处理 POST 请求。"""
        if _restricted_local():
            from agentflow_runtime.security.local_sandbox_auth import HEADER

            auth = getattr(self.server, "local_auth", None)
            if auth is None or not auth.accepts(self.headers.get(HEADER, "")):
                self._send_json(401, {"error": {
                    "code": "LOCAL_SANDBOX_AUTH_REQUIRED",
                    "message": "Local sandbox authentication required",
                }})
                return
        if _restricted_local() and self.path.startswith("/pty/"):
            self._send_json(403, {"error": {
                "code": "SECURITY_POLICY_BLOCKED",
                "message": "PTY is disabled in restricted-local mode",
            }})
            return
        if self.path == "/exec":
            self._handle_exec()
        elif self.path == "/upload":
            self._handle_upload()
        elif self.path == "/artifact/upload":
            self._handle_artifact_upload()
        elif self.path == "/artifact/download":
            self._handle_artifact_download()
        elif self.path == "/pty/open":
            self._handle_pty_open()
        elif self.path == "/pty/send":
            self._handle_pty_send()
        elif self.path == "/pty/resize":
            self._handle_pty_resize()
        elif self.path == "/pty/kill":
            self._handle_pty_kill()
        elif self.path == "/pty/read":
            self._handle_pty_read()
        else:
            self._send_json(404, {"error": {
                "code": "NOT_FOUND",
                "message": f"Not found: {self.path}",
            }})

    def _handle_artifact_download(self):
        """以原始字节下载生成产物：POST /artifact/download。

        只允许既有 Artifact 后缀，拒绝用户 upload 输入目录，并沿用工作区 realpath
        边界。内容使用 base64 放入 JSON，避免 /exec + read_file 的文本解码和截断。
        """
        try:
            request = self._read_json_body()
        except (ValueError, json.JSONDecodeError) as e:
            self._send_json(400, {"error": {
                "code": "INVALID_JSON",
                "message": f"Invalid JSON: {e}",
            }})
            return

        path = request.get("path", "")
        if not path:
            self._send_json(400, {"error": {
                "code": "TOOL_PARAMS_INVALID",
                "message": "need path",
            }})
            return

        artifact_suffixes = {
            ".png", ".jpg", ".jpeg", ".webp", ".gif", ".svg", ".bmp", ".tiff",
            ".html", ".htm", ".pdf", ".pptx", ".docx", ".xlsx",
            ".mp4", ".mp3", ".wav", ".webm", ".csv", ".json", ".yaml", ".yml",
        }
        normalized = "/" + path.replace("\\", "/").lstrip("/")
        if Path(path).suffix.lower() not in artifact_suffixes:
            self._send_json(415, {"error": {
                "code": "UNSUPPORTED_ARTIFACT_TYPE",
                "message": f"unsupported artifact type: {Path(path).suffix}",
            }})
            return
        if "/upload/" in normalized.lower() or "/uploads/" in normalized.lower():
            self._send_json(403, {"error": {
                "code": PATH_NOT_IN_UPLOAD_DIR,
                "message": "user upload inputs cannot be downloaded as generated artifacts",
            }})
            return

        workdir = os.environ.get("AF_SANDBOX_WORKSPACE") or os.getcwd()
        try:
            resolved = resolve_safe_path(path, workdir)
            content = Path(resolved).read_bytes()
        except (ValueError, PathEscapeException) as e:
            self._send_json(400, {"error": {
                "code": "PATH_OUT_OF_BOUNDS",
                "message": str(e),
                "details": {"path": path},
            }})
            return
        except OSError as e:
            self._send_json(404, {"error": {
                "code": "FILE_READ_FAILED",
                "message": f"failed to read artifact: {e}",
                "details": {"path": path},
            }})
            return

        self._send_json(200, {
            "path": path,
            "size": len(content),
            "content_b64": base64.b64encode(content).decode("ascii"),
        })

    def _handle_artifact_upload(self):
        """Write trusted generated media bytes into the sandbox workspace."""
        try:
            request = self._read_json_body()
        except (ValueError, json.JSONDecodeError) as e:
            self._send_json(400, {"error": {
                "code": "INVALID_JSON",
                "message": f"Invalid JSON: {e}",
            }})
            return

        path = request.get("path", "")
        content_b64 = request.get("content_b64", "")
        if not isinstance(path, str) or not path or not isinstance(content_b64, str):
            self._send_json(400, {"error": {
                "code": "TOOL_PARAMS_INVALID",
                "message": "need string path and content_b64",
            }})
            return

        media_suffixes = {
            ".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp", ".tiff",
            ".mp4", ".mov", ".webm", ".mp3", ".wav",
        }
        normalized = "/" + path.replace("\\", "/").lstrip("/")
        if Path(path).suffix.lower() not in media_suffixes:
            self._send_json(415, {"error": {
                "code": "UNSUPPORTED_ARTIFACT_TYPE",
                "message": f"unsupported media type: {Path(path).suffix}",
            }})
            return
        if ("/upload/" in normalized.lower()
                or _is_protected_path(path, for_write=True)):
            self._send_json(403, {"error": {
                "code": "SECURITY_POLICY_BLOCKED",
                "message": "artifact target path is protected",
            }})
            return

        try:
            content = base64.b64decode(content_b64, validate=True)
        except (ValueError, TypeError) as e:
            self._send_json(400, {"error": {
                "code": "BASE64_DECODE_FAILED",
                "message": f"base64 decode failed: {e}",
            }})
            return
        if len(content) > 256 * 1024 * 1024:
            self._send_json(413, {"error": {
                "code": "ARTIFACT_TOO_LARGE",
                "message": "artifact exceeds 256 MiB",
            }})
            return

        workdir = os.environ.get("AF_SANDBOX_WORKSPACE") or os.getcwd()
        try:
            resolved = resolve_safe_path(path, workdir)
            Path(resolved).parent.mkdir(parents=True, exist_ok=True)
            Path(resolved).write_bytes(content)
        except (ValueError, PathEscapeException) as e:
            self._send_json(400, {"error": {
                "code": "PATH_OUT_OF_BOUNDS",
                "message": str(e),
                "details": {"path": path},
            }})
            return
        except OSError as e:
            self._send_json(500, {"error": {
                "code": "FILE_WRITE_FAILED",
                "message": f"failed to write artifact: {e}",
                "details": {"path": path},
            }})
            return

        self._send_json(200, {
            "written": True,
            "bytes_written": len(content),
            "path": path,
        })


    def _handle_upload(self):
        """处理二进制资产上传：POST /upload。

        与 /exec + write_file 平级的二进制专属端点。write_file 的 content 是 str，
        JSON 字符串链路装不下二进制（NUL 字节、非 ASCII 会崩或截断），故二进制走本端点：
        base64 在 HTTP body 里传输，沙箱内解码后以 'wb' 写二进制原件。

        强制 path 为会话级路径格式 ``workspace/{user_id}_{client_id}/upload/``（守 I-6 + 防穿越）。
        契约：contracts/sandbox-http-server.md POST /upload。
        """
        try:
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length) if length > 0 else b"{}"
            request = json.loads(body.decode("utf-8"))
        except (ValueError, json.JSONDecodeError) as e:
            self._send_json(400, {"error": {
                "code": "INVALID_JSON",
                "message": f"Invalid JSON: {e}",
            }})
            return

        path = request.get("path", "")
        content_b64 = request.get("content_b64", "")
        if not path or not content_b64:
            self._send_json(400, {"error": {
                "code": "TOOL_PARAMS_INVALID",
                "message": "need path and content_b64",
            }})
            return

        # 会话级路径校验：只接受 workspace/{user_id}_{client_id}/upload/{filename}
        upload_err = validate_upload_path(path)
        if upload_err:
            self._send_json(400, {"error": {
                "code": PATH_NOT_IN_UPLOAD_DIR,
                "message": upload_err,
                "details": {"path": path},
            }})
            return

        # 解码 base64 → 二进制 bytes
        try:
            content_bytes = base64.b64decode(content_b64)
        except Exception as e:
            self._send_json(400, {"error": {
                "code": "BASE64_DECODE_FAILED",
                "message": f"base64 decode failed: {e}",
            }})
            return

        # resolve_safe_path 双检越界（B4）：workdir 基准 + realpath
        workdir = os.environ.get("AF_SANDBOX_WORKSPACE") or os.getcwd()
        try:
            resolved = resolve_safe_path(path, workdir)
        except (ValueError, PathEscapeException) as e:
            self._send_json(400, {"error": {
                "code": "PATH_OUT_OF_BOUNDS",
                "message": str(e),
                "details": {"path": path},
            }})
            return

        # 写二进制原件（创建父目录）
        try:
            Path(resolved).parent.mkdir(parents=True, exist_ok=True)
            with open(resolved, "wb") as f:
                f.write(content_bytes)
        except OSError as e:
            self._send_json(500, {"error": {
                "code": "INTERNAL",
                "message": f"failed to write file: {e}",
                "details": {"path": resolved},
            }})
            return

        self._send_json(200, {
            "written": True,
            "bytes_written": len(content_bytes),
            "path": path,
        })

    def _handle_exec(self):
        """处理工具执行：POST /exec"""
        try:
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length) if length > 0 else b"{}"
            request = json.loads(body.decode("utf-8"))
        except (ValueError, json.JSONDecodeError) as e:
            self._send_json(400, {"error": {
                "code": "INVALID_JSON",
                "message": f"Invalid JSON: {e}",
            }})
            return
        tool = request.get("tool", "")
        tool_params = request.get("tool_params", {})
        opts = request.get("opts", {})
        handler = TOOL_DISPATCH.get(tool)
        if not handler:
            self._send_json(200, _exec_result(
                1, stderr=f"Unknown tool: {tool}. Available: {list(TOOL_DISPATCH.keys())}"))
            return
        if not _concurrency_semaphore.acquire(blocking=False):
            self._send_json(503, _exec_result(
                1, stderr=f"Concurrent tool execution limit reached ({MAX_CONCURRENT}). Retry later."))
            return
        try:
            result = handler(tool, tool_params, opts)
            self._send_json(200, result)
        except PathEscapeException as e:
            self._send_json(200, _exec_result(1, stderr=str(e)))
        except Exception as e:
            self._send_json(200, _exec_result(1, stderr=f"Internal error: {type(e).__name__}: {e}"))
        finally:
            _concurrency_semaphore.release()

    # -- 交互式 PTY 端点（webshell v0.4） ----------------------------

    def _read_json_body(self) -> dict:
        """读取并解析请求 JSON body（失败抛 ValueError）。"""
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length > 0 else b"{}"
        return json.loads(body.decode("utf-8"))

    def _handle_pty_open(self):
        """POST /pty/open — 启动交互式 PTY（forkpty + exec /bin/bash -i -l）。

        请求体：{"rows": N, "cols": N, "cwd": "/path"}。
        响应：{"session_id": "...", "pid": N}。后台线程持续读 master_fd 输出，
        bridge 经 /pty/read 轮询增量（无状态转发，I-4 幂等）。
        """
        try:
            request = self._read_json_body()
        except (ValueError, json.JSONDecodeError) as e:
            self._send_json(400, {"error": {"code": "INVALID_JSON", "message": f"Invalid JSON: {e}"}})
            return
        rows = int(request.get("rows", 24))
        cols = int(request.get("cols", 80))
        cwd = request.get("cwd") or os.environ.get("AF_SANDBOX_WORKSPACE") or os.getcwd()
        session_id = uuid.uuid4().hex[:12]
        try:
            pid, master_fd = pty_mod.fork()
        except OSError as exc:
            logging.error("sandbox_http_server.pty_fork failed: %s", exc)
            self._send_json(500, {"error": {"code": "PTY_FORK_FAILED", "message": str(exc)}})
            return
        if pid == 0:
            # 子进程：exec 交互式 bash
            try:
                os.chdir(cwd)
                os.environ.setdefault("TERM", "xterm-256color")
                os.execv("/bin/bash", ["/bin/bash", "-i", "-l"])
            except Exception as exc:  # noqa: BLE001 — exec 失败退出，父进程会读到 EOF
                os._exit(127)
        # 父进程：登记会话并启动后台读线程
        _pty_set_winsize(master_fd, rows, cols)
        with _pty_sessions_lock:
            _pty_sessions[session_id] = {
                "master_fd": master_fd,
                "child_pid": pid,
                "rows": rows,
                "cols": cols,
                "output": bytearray(),
                "read_offset": 0,
                "exit_code": None,
            }
        threading.Thread(target=_pty_reader, args=(session_id,), daemon=True).start()
        logging.info("sandbox_http_server.pty_open session_id=%s pid=%s", session_id, pid)
        self._send_json(200, {"session_id": session_id, "pid": pid})

    def _handle_pty_send(self):
        """POST /pty/send — 向 PTY 写入输入（base64，含 Ctrl+C 字节 \\x03）。"""
        try:
            request = self._read_json_body()
        except (ValueError, json.JSONDecodeError) as e:
            self._send_json(400, {"error": {"code": "INVALID_JSON", "message": f"Invalid JSON: {e}"}})
            return
        session_id = request.get("session_id", "")
        with _pty_sessions_lock:
            session = _pty_sessions.get(session_id)
        if session is None:
            self._send_json(404, {"error": {"code": "PTY_NOT_FOUND", "message": f"pty session not found: {session_id}"}})
            return
        try:
            data = base64.b64decode(request.get("data", ""))
            os.write(session["master_fd"], data)
        except (ValueError, OSError) as exc:
            logging.error("sandbox_http_server.pty_send failed session_id=%s: %s", session_id, exc)
            self._send_json(500, {"error": {"code": "PTY_SEND_FAILED", "message": str(exc)}})
            return
        self._send_json(200, {"ok": True})

    def _handle_pty_resize(self):
        """POST /pty/resize — 调整 PTY 窗口尺寸（xterm resize 事件）。"""
        try:
            request = self._read_json_body()
        except (ValueError, json.JSONDecodeError) as e:
            self._send_json(400, {"error": {"code": "INVALID_JSON", "message": f"Invalid JSON: {e}"}})
            return
        session_id = request.get("session_id", "")
        with _pty_sessions_lock:
            session = _pty_sessions.get(session_id)
        if session is None:
            self._send_json(404, {"error": {"code": "PTY_NOT_FOUND", "message": f"pty session not found: {session_id}"}})
            return
        rows = int(request.get("rows", 24))
        cols = int(request.get("cols", 80))
        _pty_set_winsize(session["master_fd"], rows, cols)
        with _pty_sessions_lock:
            session["rows"] = rows
            session["cols"] = cols
        self._send_json(200, {"ok": True})

    def _handle_pty_kill(self):
        """POST /pty/kill — 终止 PTY 会话（WebSocket 断开/关闭面板时调用）。

        幂等：会话不存在返回 200（对齐 kill 幂等语义，I-4）。
        """
        try:
            request = self._read_json_body()
        except (ValueError, json.JSONDecodeError) as e:
            self._send_json(400, {"error": {"code": "INVALID_JSON", "message": f"Invalid JSON: {e}"}})
            return
        session_id = request.get("session_id", "")
        with _pty_sessions_lock:
            session = _pty_sessions.pop(session_id, None)
        if session is None:
            self._send_json(200, {"ok": True, "already_gone": True})
            return
        try:
            os.kill(session["child_pid"], signal.SIGKILL)
        except ProcessLookupError:
            pass
        logging.info("sandbox_http_server.pty_kill session_id=%s pid=%s", session_id, session["child_pid"])
        self._send_json(200, {"ok": True})

    def _handle_pty_read(self):
        """POST /pty/read — 增量读取 PTY 输出（bridge 轮询）。

        请求体：{"session_id": "...", "offset": N}。
        响应：{"stdout": "...", "stderr": "", "offset": N, "exit_code": null|int}。
        exit_code 非 null 表示 PTY 进程已结束（route 层发送 exit 帧并终止轮询）。
        """
        try:
            request = self._read_json_body()
        except (ValueError, json.JSONDecodeError) as e:
            self._send_json(400, {"error": {"code": "INVALID_JSON", "message": f"Invalid JSON: {e}"}})
            return
        session_id = request.get("session_id", "")
        with _pty_sessions_lock:
            session = _pty_sessions.get(session_id)
        if session is None:
            self._send_json(404, {"error": {"code": "PTY_NOT_FOUND", "message": f"pty session not found: {session_id}"}})
            return
        offset = int(request.get("offset", 0))
        with _pty_sessions_lock:
            output = bytes(session["output"])
            exit_code = session["exit_code"]
        if offset < len(output):
            chunk = output[offset:]
            new_offset = len(output)
        else:
            chunk = b""
            new_offset = offset
        self._send_json(200, {
            "stdout": chunk.decode("utf-8", errors="replace"),
            "stderr": "",
            "offset": new_offset,
            "exit_code": exit_code,
        })

    def _send_json(self, status: int, data: dict):
        """发送 JSON 响应。"""
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        """使用 logging 模块而非 stderr 直写。"""
        logging.info("sandbox_http_server: %s - %s", self.address_string(), fmt % args)


def _parent_monitor(ppid: int):
    """父进程监控线程：父进程退出时自动退出沙箱 HTTP server。"""
    while True:
        time.sleep(2)
        try:
            os.kill(ppid, 0)
        except OSError:
            logging.warning("Parent process %d died, shutting down sandbox HTTP server", ppid)
            os._exit(0)


def main():
    global _SECURITY_PROFILE
    parser = argparse.ArgumentParser(description="Sandbox HTTP Server")
    parser.add_argument("--port", type=int, default=8080, help="监听端口")
    parser.add_argument("--listen-fd", type=int, default=None,
                        help="LocalSandbox 已绑定的监听 socket，避免并发选端口竞争")
    parser.add_argument("--host", default="0.0.0.0", help="监听地址（默认 0.0.0.0）")
    parser.add_argument("--parent-pid", type=int, default=0,
                        help="父进程 PID（非 0 时启动监控线程）")
    parser.add_argument(
        "--security-profile",
        choices=("isolated", "restricted-local"),
        default="isolated",
        help="执行安全配置；宿主进程模式必须使用 restricted-local",
    )
    args = parser.parse_args()
    _SECURITY_PROFILE = args.security_profile
    if _restricted_local():
        _scrub_restricted_local_environment()

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        stream=sys.stderr,
    )

    # HTTP server 是沙箱工具执行链的终点。若继续继承 worker 用来访问本服务的
    # endpoint，runtime tool 会再次 POST 回本服务，递归耗尽并发槽。
    inherited_mode = os.environ.get("AF_SANDBOX_MODE", "")
    inherited_endpoint = os.environ.get("AF_SANDBOX_ENDPOINT", "")
    os.environ["AF_SANDBOX_MODE"] = "none"
    os.environ.pop("AF_SANDBOX_ENDPOINT", None)
    if inherited_mode or inherited_endpoint:
        logging.info(
            "sandbox_http_server cleared upstream executor routing "
            "(inherited_mode=%s endpoint_present=%s)",
            inherited_mode or "<unset>",
            bool(inherited_endpoint),
        )

    if args.parent_pid:
        threading.Thread(target=_parent_monitor, args=(args.parent_pid,), daemon=True).start()

    if args.listen_fd is None:
        server = ThreadingHTTPServer((args.host, args.port), SandboxHttpHandler)
    else:
        server = ThreadingHTTPServer((args.host, args.port), SandboxHttpHandler,
                                     bind_and_activate=False)
        server.socket.close()
        server.socket = socket.socket(fileno=args.listen_fd)
        server.socket.set_inheritable(False)
        server.server_address = server.socket.getsockname()
        server.server_name = socket.getfqdn(server.server_address[0])
        server.server_port = server.server_address[1]
        server.server_activate()

    def shutdown(signum, frame):
        logging.info("Received signal %d, shutting down", signum)
        sys.exit(0)

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    local_auth = None
    try:
        if _restricted_local():
            from agentflow_runtime.security.local_sandbox_auth import LocalSandboxAuth

            local_auth = LocalSandboxAuth(server.server_port)
            server.local_auth = local_auth
        logging.info("sandbox_http_server listening on %s:%d (parent=%d)",
                     args.host, server.server_port, args.parent_pid)
        server.serve_forever()
    except KeyboardInterrupt:
        server.shutdown()
    finally:
        server.server_close()
        if local_auth is not None:
            local_auth.close()


if __name__ == "__main__":
    main()
