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
import importlib
import json
import logging
import os
import re
import signal
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

# 默认输出截断阈值（0=不截断，由 daemon 通过 opts 注入实际值）
DEFAULT_OUTPUT_MAX_BYTES = 0

# 并发执行上限
MAX_CONCURRENT = int(os.environ.get("SANDBOX_MAX_CONCURRENT", "8"))
_concurrency_semaphore = threading.Semaphore(MAX_CONCURRENT)

# 错误码常量（与 contracts/sandbox-http-server.md 对齐）
PATH_NOT_IN_UPLOAD_DIR = "PATH_NOT_IN_UPLOAD_DIR"

# 危险命令前缀（宿主侧第一道防线，B2）
DANGEROUS_PREFIXES = (
    "rm -rf /", "sudo ", "sudo\t", ":(){", "fork bomb",
    "mkfs.", "dd if=/dev/zero of=/dev/sd", "shutdown", "reboot",
)

# FR-003 已变更(见 docs/design/features/sandbox-env-injection.md §4):
# 密钥 *_API_KEY 允许在沙箱内文件中,不再用宿主侧白名单拦截。
# env_vars 透传到沙箱——宿主机 os.environ 不透传(由 e2b.py 的 set -a && source .env 兜底)。


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
    - ``tool=read_file/write_file/edit_file`` → 先检查 ``resolve_safe_path()``
    """
    workdir = opts.get("working_dir") or os.environ.get("SANDBOX_WORKDIR") or os.getcwd()
    output_max_bytes = opts.get("output_max_bytes", DEFAULT_OUTPUT_MAX_BYTES)

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

    elif tool_name in ("read_file", "write_file", "edit_file"):
        old_path = tool_params.get("path", "")
        try:
            resolve_safe_path(old_path, workdir)
        except (PathEscapeException, ValueError) as e:
            return _exec_result(1, stderr=str(e))

    elif tool_name == "grep":
        pattern = tool_params.get("pattern", "")
        try:
            re.compile(pattern)
        except re.error as e:
            return _exec_result(1, stderr=f"Invalid regex: {e}")

    # -- 构建 tool_input（翻译参数名 + 合并 opts 中的相关字段） --
    tool_input = {}
    for k, v in tool_params.items():
        mapped = _PARAM_MAP.get((tool_name, k), k)
        tool_input[mapped] = v
    if "timeout_ms" in opts:
        tool_input["timeout"] = opts["timeout_ms"] // 1000

    # -- cwd override（webshell 跨命令 cwd 延续）：opts.working_dir 优先于默认 agent package dir --
    # 仅对 bash 生效——read_file/write_file/edit_file 走 resolve_safe_path 基准 workdir（上面已取）
    if tool_name == "bash" and opts.get("working_dir"):
        tool_input["cwd"] = opts["working_dir"]

    # FR-003 已变更(见 docs/design/features/sandbox-env-injection.md §4):
    # env_vars 不再白名单拦截——密钥 *_API_KEY 允许透传到沙箱内(可信沙箱持密)。
    env_vars = opts.get("env_vars", {})
    if env_vars:
        tool_input["env_vars"] = env_vars

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
            timeout_secs = tool_input.get("timeout", 0)
            if timeout_secs and timeout_secs > 0:
                with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                    future = pool.submit(mod.execute, tool_input)
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
                result_json = mod.execute(tool_input)
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
    elif tool_name in ("read_file", "load_skill"):
        # runtime 返回 {"content": "...", "error": null}
        stdout = result.get("content") or ""
        stderr = error
        truncated = False
    elif tool_name in ("write_file", "edit_file"):
        # runtime 返回 {"success": true, "error": null, "bytes_written": N}
        stdout = "" if result.get("success", True) else error
        stderr = "" if result.get("success", True) else error
        exit_code = 0 if result.get("success", True) else 1
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


# tool 分发表（所有工具统一委托 agentflow_runtime.harness.tools.*）
TOOL_DISPATCH = {
    "bash": _exec_tool,
    "read_file": _exec_tool,
    "write_file": _exec_tool,
    "edit_file": _exec_tool,
    "list_skills": _exec_tool,
    "load_skill": _exec_tool,
    "grep": _exec_tool,
    "glob": _exec_tool,
}


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
        if self.path == "/exec":
            self._handle_exec()
        elif self.path == "/upload":
            self._handle_upload()
        else:
            self._send_json(404, {"error": {
                "code": "NOT_FOUND",
                "message": f"Not found: {self.path}",
            }})

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
        workdir = os.environ.get("SANDBOX_WORKDIR") or os.getcwd()
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
    parser = argparse.ArgumentParser(description="Sandbox HTTP Server")
    parser.add_argument("--port", type=int, default=8080, help="监听端口")
    parser.add_argument("--host", default="0.0.0.0", help="监听地址（默认 0.0.0.0）")
    parser.add_argument("--parent-pid", type=int, default=0,
                        help="父进程 PID（非 0 时启动监控线程）")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s",
        stream=sys.stderr,
    )

    if args.parent_pid:
        threading.Thread(target=_parent_monitor, args=(args.parent_pid,), daemon=True).start()

    server = ThreadingHTTPServer((args.host, args.port), SandboxHttpHandler)
    logging.info("sandbox_http_server listening on 0.0.0.0:%d (parent=%d)",
                 args.port, args.parent_pid)

    def shutdown(signum, frame):
        logging.info("Received signal %d, shutting down", signum)
        sys.exit(0)

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        server.shutdown()


if __name__ == "__main__":
    main()
