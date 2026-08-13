"""
E2E 测试共享辅助函数 — daemon 生命周期 + mock LLM + JSON-RPC 通信。

设计原则：
  1. 每个 test class 启动自己的 daemon（≤5 次请求后 daemon 退化，class scope 避免累积）
  2. 使用 mock LLM server（e2e-scripted），不依赖真实 API
  3. 验证执行链完整性（节点 Exit code=0、response 送达），不做 LLM 输出内容断言
  4. fixture 确保 daemon 在 teardown 时被终止
"""

from __future__ import annotations

import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Iterator

import pytest

PROJECT_ROOT = Path(__file__).resolve().parents[3]
DAEMON_BINARY = PROJECT_ROOT / "build" / "agentflowd"

# 三态模型（agent-package-three-state.md §2.1）：
#   ② 固化态 = resource/agent-packages/outfit-assistant/（纯净静态资产 + .env）
#   ③ 运行态 = examples/agent/outfit-assistant/（② 拷贝 + 运行时数据 .venv/outputs/…）
# 不变量（用户拍板 2026-08-07）：删 examples/agent/ 不应影响功能/测试/脚本——
# 测试首次用时自动从 ② 同步重建 ③，daemon CLI 仍加载 ③ 路径。
OUTFIT_AGENT = PROJECT_ROOT / "examples" / "agent" / "outfit-assistant"
OUTFIT_AGENT_STR = str(OUTFIT_AGENT)  # absolute string path for daemon CLI
OUTFIT_AGENT_2 = PROJECT_ROOT / "resource" / "agent-packages" / "outfit-assistant"
LLM_REPLY = "你好，我是服装搭配助手，很高兴为您服务！"

# 测试 daemon/bridge 日志统一落到 logs/tests/（项目根下，gitignored）
# 结构: logs/tests/<YYYYMMDD_HHMMSS>_<cls>_<fixture>/daemon.log
# 与 logs/*.log 同目录管理,避免 pytest tmp_path 自动清理导致现场证据丢失
_TESTS_LOG_ROOT = PROJECT_ROOT / "logs" / "tests"

# 测试 daemon sandbox 配置路径（同 daemon 默认 fallback 路径）
_TEST_SANDBOX_CONFIG = PROJECT_ROOT / "configs" / "test" / "agentflowd.json"


def _rsync_2_to_3_if_needed(target_3: Path = OUTFIT_AGENT, source_2: Path = OUTFIT_AGENT_2) -> bool:
    """删 examples 不崩不变量：③ 缺失或 ② 比 ③ 新时自动 rsync ②→③。

    排除运行时数据（.venv/outputs/workspace/.agentfoundry/preferences.json/manifest.jsonc/dist），
    保留 ③ 已有运行时数据。幂等（rsync --update 仅传新/改文件）。
    返回 True 表示发生了同步，False 表示 ③ 已就绪跳过。

    设计：agent-package-three-state.md §5（与 start_daemon.sh / start_dev.sh /
    start_production.sh 的 _sync_②_to_③ 同源逻辑，单一真相源）。
    """
    import shutil
    if not source_2.is_dir():
        return False  # ② 缺失由调用方判定（② 是权威源，缺则无法重建）
    need_sync = (
        not target_3.is_dir()
        or not (target_3 / "agent.json").is_file()
        or source_2.stat().st_mtime > target_3.stat().st_mtime
    )
    if not need_sync:
        return False
    target_3.mkdir(parents=True, exist_ok=True)
    _EXCLUDES = {
        ".venv", "outputs", "workspace", ".agentfoundry",
        "preferences.json", "manifest.jsonc", "dist",
    }
    shutil.copytree(
        source_2, target_3, dirs_exist_ok=True,
        ignore=shutil.ignore_patterns(*_EXCLUDES),
    )
    return True


def ensure_outfit_agent_3() -> Path:
    """确保 ③ 运行实例就绪——删 examples 后测试首次用时自动从 ② 重建。

    返回 OUTFIT_AGENT（③ 路径）。② 缺则 raise（② 是权威源，缺即无法重建）。
    """
    if not OUTFIT_AGENT_2.is_dir():
        raise RuntimeError(
            f"② 固化态缺失，无法重建 ③: {OUTFIT_AGENT_2} "
            "(resource/agent-packages/outfit-assistant 应存在)"
        )
    _rsync_2_to_3_if_needed()
    return OUTFIT_AGENT


def _load_sandbox_mode() -> str:
    """从 configs/test/agentflowd.json 读取 sandbox.mode，默认返回 'docker'。"""
    try:
        with open(_TEST_SANDBOX_CONFIG) as f:
            cfg = json.load(f)
        mode = cfg.get("sandbox", {}).get("mode", "docker")
        return str(mode)
    except (FileNotFoundError, json.JSONDecodeError, KeyError):
        return "docker"


# ═══════════════════════════════════════════════════════════════════
# E2E agent layer selection — 通过 E2E_AGENT_LAYERS 控制哪些层测试执行
# ═══════════════════════════════════════════════════════════════════

# 层定义
AGENT_LAYER_L0 = 0  # agent-connectivity
AGENT_LAYER_L1 = 1  # agent-sandbox
AGENT_LAYER_L2 = 2  # agent-external
AGENT_LAYER_L3 = 3  # agent-outfit

# 默认启用的层（未设 E2E_AGENT_LAYERS 时）
_DEFAULT_LAYERS = {AGENT_LAYER_L0, AGENT_LAYER_L1}


def _get_enabled_layers() -> set[int]:
    """从环境变量 E2E_AGENT_LAYERS 解析启用的层集合。

    格式：逗号分隔的数字，如 "0,1,2,3"
    未设置时默认启用 L0 + L1（向后兼容，不需 API key）。
    """
    raw = os.environ.get("E2E_AGENT_LAYERS", "")
    if not raw:
        return _DEFAULT_LAYERS.copy()
    try:
        layers = set()
        for part in raw.split(","):
            part = part.strip()
            if part:
                layers.add(int(part))
        return layers
    except (ValueError, TypeError):
        return _DEFAULT_LAYERS.copy()


def skip_if_layer_not_in(layer: int) -> bool | str:
    """返回 pytest skip 条件，当指定层不在 E2E_AGENT_LAYERS 中时返回原因字符串。

    返回值可直接用作 ``pytest.mark.skipif`` 的条件和 reason：:

        reason = skip_if_layer_not_in(AGENT_LAYER_L1)
        pytestmark = [
            pytest.mark.e2e,
            pytest.mark.skipif(reason, reason=reason),
        ]

    当层在启用集合中时返回 ``False``（不跳过），
    不在时返回原因字符串（可同时作为 condition 和 reason）。
    """
    enabled = _get_enabled_layers()
    if layer in enabled:
        return False
    layer_names = {
        AGENT_LAYER_L0: "L0(agent-connectivity)",
        AGENT_LAYER_L1: "L1(agent-sandbox)",
        AGENT_LAYER_L2: "L2(agent-external)",
        AGENT_LAYER_L3: "L3(agent-outfit)",
    }
    name = layer_names.get(layer, f"L{layer}")
    enabled_str = ",".join(str(l) for l in sorted(enabled))
    return f"Layer {name} not in E2E_AGENT_LAYERS={enabled_str}"


def _test_log_dir(fixture: str, cls_name: str) -> Path:
    """为单个 fixture 启动创建日志目录: logs/tests/<ts>_<cls>_<fixture>/.

    时间戳精确到秒。每次创建新目录前，清理该 (cls, fixture) 组合的旧日志目录，
    只保留最近 3 次，避免测试日志撑爆磁盘。
    """
    # 清理旧日志：只保留最近 3 次同组合的日志
    _cleanup_old_logs(cls_name, fixture)

    ts = time.strftime("%Y%m%d_%H%M%S")
    dir_ = _TESTS_LOG_ROOT / f"{ts}_{cls_name}_{fixture}"
    dir_.mkdir(parents=True, exist_ok=True)
    return dir_


def _cleanup_old_logs(cls_name: str, fixture: str) -> None:
    """清理同 (cls_name, fixture) 组合的旧日志目录，只保留最近 3 次。"""
    if not _TESTS_LOG_ROOT.exists():
        return
    prefix = f"_{cls_name}_{fixture}"
    entries = sorted(
        [d for d in _TESTS_LOG_ROOT.iterdir() if d.is_dir() and d.name.endswith(prefix)],
        key=lambda d: d.name,
        reverse=True,  # 最新的在前
    )
    # 保留最近 3 次，删除更旧的
    for old_dir in entries[3:]:
        import shutil
        shutil.rmtree(old_dir, ignore_errors=True)


def _daemon_log_path(log_dir: Path, name: str = "daemon.log") -> Path:
    """返回 daemon log 完整路径,并在父目录已存在的前提下返回 Path 对象.

    调用方需用 ``Path.open("wb")`` 拿到 file object 传给 Popen stdout.
    """
    return log_dir / name


def _daemon_env() -> dict[str, str]:
    """构建 daemon 子进程环境，确保系统 libc++ 优先于 conda 旧版。"""
    env = dict(os.environ)
    # macOS: conda 的 libc++（2022）缺少 __from_native_exception_pointer 符号
    # 将 /usr/lib（系统 libc++）插入 DYLD_LIBRARY_PATH 最前面
    if sys.platform == "darwin":
        lib_path = env.get("DYLD_LIBRARY_PATH", "")
        sys_libs = "/usr/lib"
        if lib_path:
            parts = [p for p in lib_path.split(":") if p != sys_libs]
            env["DYLD_LIBRARY_PATH"] = sys_libs + ":" + ":".join(parts)
        else:
            env["DYLD_LIBRARY_PATH"] = sys_libs
    return env


def free_port() -> int:
    """获取随机可用端口。"""
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


# ═══════════════════════════════════════════════════════════════════
# Mock LLM 服务器（返回固定回复 e2e-scripted）
# ═══════════════════════════════════════════════════════════════════


class _ScriptedLLMHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("content-length", "0"))
        body = self.rfile.read(length).decode()
        import json as _json
        _req = _json.loads(body) if body else {}
        _stream = _req.get("stream", False)
        _messages = _req.get("messages", [])
        _tools = _req.get("tools", [])
        # 检查所有用户消息中是否包含图片关键词（skill_executor 会在末尾追加系统提醒，所以不能只看最后一条）
        _user_msgs = [m.get("content", "") for m in _messages if isinstance(m, dict) and m.get("role") == "user"]
        _all_user_content = " ".join(_user_msgs)
        _last_user_msg = _user_msgs[-1] if _user_msgs else ""
        _has_image_request = "图片" in _all_user_content or "生成" in _all_user_content
        # 检测是否是技能结果回传（避免递归循环：coordinator 收到结果后又调度一次 draw-image）
        _skill_done = "成功生成" in _last_user_msg or "draw-image" in _last_user_msg and "已完成" in _last_user_msg
        # 检查 tools 是否包含 bash 来判断来源：coordinator(无bash) → run_skills，skill_executor(有bash) → 文本响应
        _has_bash_tool = any(
            t.get("function", {}).get("name") == "bash"
            for t in _tools if isinstance(t, dict)
        )

        if _has_bash_tool:
            # skill_executor 场景
            # 检查所有消息中是否有 bash 工具执行结果（bash 结果可能以 tool/function 角色添加）
            _all_content = " ".join(m.get("content", "") or "" for m in _messages if isinstance(m, dict))
            if '"tool": "bash"' in _all_content or '"exit_code"' in _all_content:
                # bash 已执行完毕，返回包含 artifacts_json 的 JSON 响应
                _msg = {"role": "assistant", "content": _json.dumps({
                    "skill_result": "成功生成测试图片",
                    "artifacts_json": _json.dumps([{"path": "test_image.png", "type": "image/png"}])
                })}
                _finish = "stop"
            else:
                # 首次调用，返回 bash 工具调用执行命令
                _dummy_args = _json.dumps({"command": "mkdir -p outputs && python3 -c \"import struct,zlib;W=50;H=50;raw=b'\\\\x00'+b'\\\\xff\\\\x00\\\\x00'*W;img=raw*H;open('outputs/test_image.png','wb').write(b'\\\\x89PNG\\\\r\\\\n\\\\x1a\\\\n'+struct.pack('>I',13)+b'IHDR'+struct.pack('>IIBBBBB',W,H,8,2,0,0,0)+struct.pack('>I',zlib.crc32(b'IHDR'+struct.pack('>IIBBBBB',W,H,8,2,0,0,0))&0xffffffff)+struct.pack('>I',len(img))+b'IDAT'+zlib.compress(img)+struct.pack('>I',zlib.crc32(b'IDAT'+zlib.compress(img))&0xffffffff)+struct.pack('>I',0)+b'IEND'+struct.pack('>I',zlib.crc32(b'IEND')&0xffffffff))\" && realpath outputs/test_image.png"})
                _msg = {
                    "role": "assistant",
                    "content": None,
                    "tool_calls": [{
                        "id": "call_e2e_tool",
                        "type": "function",
                        "function": {
                            "name": "bash",
                            "arguments": _dummy_args
                        }
                    }]
                }
                _finish = "tool_calls"
        elif _has_image_request and not _skill_done:
            # 用户消息含图片关键词 → 返回 run_skills 工具调用（调度 draw-image）
            _tool_args = _json.dumps({
                "prompt": _last_user_msg,
                "skills": [{
                    "name": "draw-image",
                    "prompt": _last_user_msg,
                    "load_skills": []
                }],
                "reason": "用户请求生成图片"
            })
            _msg = {
                "role": "assistant",
                "content": None,
                "tool_calls": [{
                    "id": "call_e2e_img",
                    "type": "function",
                    "function": {
                        "name": "run_skills",
                        "arguments": _tool_args
                    }
                }]
            }
            _finish = "tool_calls"
        else:
            _msg = {"role": "assistant", "content": LLM_REPLY}
            _finish = "stop"

        if _stream:
            # SSE 流式响应 — 直接通过 socket 发送
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Connection", "close")
            self.end_headers()
            _chunk = _json.dumps({
                "choices": [{"delta": _msg, "index": 0}],
                "created": 1, "id": "chatcmpl-e2e", "model": "e2e-scripted",
            })
            self.request.sendall(f"data: {_chunk}\n\n".encode())
            _done = _json.dumps({
                "choices": [{"delta": {}, "finish_reason": _finish, "index": 0}],
                "created": 1, "id": "chatcmpl-e2e", "model": "e2e-scripted",
            })
            self.request.sendall(f"data: {_done}\n\n".encode())
            self.request.sendall(b"data: [DONE]\n\n")
        else:
            # 非流式 JSON 响应
            _usage = {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}
            payload = _json.dumps({
                "id": "chatcmpl-e2e",
                "object": "chat.completion",
                "created": 1,
                "model": "e2e-scripted",
                "choices": [{"index": 0, "message": _msg, "finish_reason": _finish}],
                "usage": _usage,
            }).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

    def log_message(self, fmt, *args) -> None:
        pass


@contextmanager
def scripted_llm() -> Iterator[str]:
    """启动 mock LLM HTTP server，yield base URL。"""
    port = free_port()
    server = ThreadingHTTPServer(("127.0.0.1", port), _ScriptedLLMHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{port}/v1"
    finally:
        server.shutdown()
        server.server_close()


# ═══════════════════════════════════════════════════════════════════
# Daemon 生命周期
# ═══════════════════════════════════════════════════════════════════


def wait_port(port: int, process: subprocess.Popen, timeout: float = 15) -> None:
    """等待 TCP 端口就绪。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"daemon exited (rc={process.returncode})")
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(0.5)
                s.connect(("127.0.0.1", port))
                return
        except (ConnectionRefusedError, OSError):
            pass
        time.sleep(0.2)
    raise RuntimeError(f"port {port} not ready within {timeout}s")


def await_agent_ready(port: int, agent_id: str, process: subprocess.Popen,
                      timeout: float = 20) -> None:
    """等待 agent 注册完成（通过 agent.list JSON-RPC）。"""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"daemon exited (rc={process.returncode})")
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.settimeout(1)
                s.connect(("127.0.0.1", port))
                req = json.dumps({
                    "jsonrpc": "2.0", "method": "agent.list",
                    "params": {}, "id": 1,
                })
                s.sendall((req + "\n").encode("utf-8"))
                chunks = []
                while True:
                    ch = s.recv(1)
                    if not ch or ch == b"\n":
                        break
                    chunks.append(ch)
                resp = json.loads(b"".join(chunks).decode("utf-8"))
                agents = resp.get("result", {}).get("agents", [])
                if any(a.get("id") == agent_id or a.get("agent_id") == agent_id for a in agents):
                    return
        except (ConnectionRefusedError, OSError, json.JSONDecodeError, KeyError):
            pass
        time.sleep(0.3)
    raise RuntimeError(f"agent {agent_id} not ready within {timeout}s")


@pytest.fixture(scope="class")
def outfit_daemon_real_llm(request: pytest.FixtureRequest,
                           tmp_path_factory: pytest.TempPathFactory) -> Iterator[dict[str, Any]]:
    """启动 outfit-assistant daemon，优先使用真实 LLM（class-scoped）.

    class scope 合理：所有测试方法共享同一 daemon，避免 function scope
    每测试启 daemon 的 12-18s 开销 + dialogue lock 串行根因（旧 dialogue
    的 coordinator 慢会卡住新 dialogue 的 dispatcher，见 Fix 3 决策）。
    pytest fixture teardown 在所有测试方法跑完后才执行，期间不会 teardown
    daemon，所以 class scope 不会触发 "daemon 死后 bridge 仍发请求" 的挂起
    （PROJECT_MEMORY pitfall 11 那是手动杀 daemon 场景，不是 fixture teardown）。

    如果宿主机环境变量设置了 ``AF_LLM_BASE_URL`` 和 ``AF_LLM_API_KEY``，
    则透传给 daemon，发起真实 LLM 调用；否则回退到 mock LLM。

    验证完整的 agentflow 执行链路：daemon → agent → workflow → LLM call。
    """
    if not DAEMON_BINARY.is_file():
        pytest.skip(f"daemon binary not built: {DAEMON_BINARY}")

    daemon_port = free_port()
    # 日志落到 logs/tests/（项目根下，gitignored），不再用 pytest tmp_path
    log_dir = _test_log_dir("outfit_daemon_real_llm", request.cls.__name__)
    daemon_log = _daemon_log_path(log_dir)

    # 检查宿主机是否有真实 LLM 配置
    host_api_key = os.environ.get("AF_LLM_API_KEY", "")
    host_base_url = os.environ.get("AF_LLM_BASE_URL", "")
    host_model = os.environ.get("AF_LLM_MODEL", "gpt-4o-mini")
    use_real_llm = bool(host_api_key and host_base_url)

    if use_real_llm:
        # 真实 LLM 模式：透传环境变量
        env = _daemon_env()
        env["PATH"] = (
            f"{OUTFIT_AGENT / '.venv' / 'bin'}"
            f"{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"
        )
        env.setdefault("AF_LLM_BASE_URL", host_base_url)
        env.setdefault("AF_LLM_MODEL", host_model)
        env.setdefault("AF_LLM_API_KEY", host_api_key)
        env["AF_SKILL_MAX_COMPLETION_TOKENS"] = "512"
        env["NO_PROXY"] = "127.0.0.1,localhost"
        env["no_proxy"] = "127.0.0.1,localhost"
        # 节点脚本从 agentflow_runtime pip 包 import，需将包源码目录加入 Python 路径
        env.setdefault("PYTHONPATH", str(PROJECT_ROOT / "packages" / "agentflow-runtime" / "src"))
        # 透传 daemon log 路径给 exe_loader.cpp（getenv("AF_LOG_FILE") 拿到 daemon log 目录，
        # skill_stderr.<pid>.log 落同目录而非 /tmp/，遵守 PROJECT_MEMORY pitfall 10
        # 「测试日志必须落在本项目可见的目录，不能放在 /tmp/」）
        # 注意：AF_LOG_FILE 指向 daemon_log 所在目录，但 daemon 自身日志通过 stdout 写入
        # daemon_log（Logger::log() 写 stdout），避免 AF_LOG_FILE 与 stdout 指向同一文件导致双写
        env["AF_LOG_FILE"] = str(daemon_log)

        daemon = subprocess.Popen(
            [str(DAEMON_BINARY), "--listen", f"127.0.0.1:{daemon_port}",
             "--package", OUTFIT_AGENT_STR],
            cwd=PROJECT_ROOT, env=env,
            stdout=daemon_log.open("wb"),
            stderr=subprocess.STDOUT,
        )
        llm_mode = "REAL"
    else:
        # 回退到 mock LLM
        ctx = scripted_llm()
        llm_url = ctx.__enter__()
        _pythonpath = str(PROJECT_ROOT / "packages" / "agentflow-runtime" / "src")
        env = _daemon_env()
        env.update({
            "PATH": (
                f"{OUTFIT_AGENT / '.venv' / 'bin'}"
                f"{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"
            ),
            "AF_LLM_API_KEY": "e2e-test-key",
            "AF_LLM_BASE_URL": llm_url,
            "AF_LLM_MODEL": "e2e-scripted",
            "AF_SKILL_MAX_COMPLETION_TOKENS": "512",
            "NO_PROXY": "127.0.0.1,localhost",
            "no_proxy": "127.0.0.1,localhost",
            # 节点脚本从 agentflow_runtime pip 包 import，需将包源码目录加入 Python 路径
            "PYTHONPATH": _pythonpath,
        })
        daemon = subprocess.Popen(
            [str(DAEMON_BINARY), "--listen", f"127.0.0.1:{daemon_port}",
             "--package", OUTFIT_AGENT_STR],
            cwd=PROJECT_ROOT, env=env,
            stdout=daemon_log.open("wb"),
            stderr=subprocess.STDOUT,
        )
        llm_mode = "MOCK"
        # 注意：ctx.__exit__ 要在 finally 中处理

    try:
        wait_port(daemon_port, daemon)
        await_agent_ready(daemon_port, "outfit-assistant", daemon)
        yield {"daemon_port": daemon_port, "daemon": daemon,
               "llm_mode": llm_mode, "use_real_llm": use_real_llm,
               "log_path": str(daemon_log)}
    finally:
        if daemon.poll() is None:
            daemon.send_signal(signal.SIGTERM)
            try:
                daemon.wait(timeout=12)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait(timeout=3)
        if not use_real_llm:
            try:
                ctx.__exit__(None, None, None)
            except Exception:
                pass


# Backward-compatible alias: the original outfit_daemon fixture is now
# outfit_daemon_real_llm.  Tests that import outfit_daemon (outfit_agent,
# outfit_multiturn, outfit_skill) get the same fixture.
outfit_daemon = outfit_daemon_real_llm


# ═══════════════════════════════════════════════════════════════════
# JSON-RPC 通信
# ═══════════════════════════════════════════════════════════════════


# ═══════════════════════════════════════════════════════════════════
# JSON-RPC 通信
# ═══════════════════════════════════════════════════════════════════


def jsonrpc_call(host: str, port: int, method: str,
                 params: dict, req_id: int = 1) -> dict:
    """发送 JSON-RPC 请求到 daemon，返回 \n 分隔的单行响应。"""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(30)
        sock.connect((host, port))
        payload = json.dumps({
            "jsonrpc": "2.0", "method": method,
            "params": params, "id": req_id,
        })
        sock.sendall((payload + "\n").encode("utf-8"))
        chunks = []
        while True:
            ch = sock.recv(1)
            if not ch or ch == b"\n":
                break
            chunks.append(ch)
        return json.loads(b"".join(chunks).decode("utf-8"))


def send_user_message(host: str, port: int, content: str,
                      task_id: str = "e2e-outfit-001") -> dict:
    """通过 daemon 直接发送用户消息（JSON-RPC agent.execute），返回响应。"""
    return jsonrpc_call(host, port, "agent.execute", {
        "agent_id": "outfit-assistant",
        "user_id": "e2e-user",
        "client_id": task_id,
        "content": content,
        "request_id": task_id,
    }, task_id)


def poll_agent_response(host: str, port: int, task_id: str,
                        timeout: int = 120) -> str:
    """轮询 daemon 的 agent.poll_response，等待 task 完成，返回最终消息内容。

    注意事项：
      agent.poll_response 在 daemon 中存在已知限制：
      - 消息可能被 ResponsePoller（50ms 周期）异步通知消费
      - 消费后 poll 返回 messages=[]，但 complete 仍为 true
        （complete = 无运行节点 && 响应队列空，2026-08-10 修正，不再依赖 msg_count）
      - 因此需要高频轮询，在 ResponsePoller 消费前捕获消息
      - complete=true 且无消息 = 任务真实完成但响应已被异步消费 → 返回空
    """
    deadline = time.monotonic() + timeout
    poll_count = 0
    while time.monotonic() < deadline:
        poll_count += 1

        # 高频 poll（每 100ms），尝试在 ResponsePoller 消费前捕获
        resp = jsonrpc_call(host, port, "agent.poll_response", {
            "agent_id": "outfit-assistant",
            "user_id": "e2e-user",
            "client_id": task_id,
            "timeout_ms": "500",
        }, req_id=2)
        result = resp.get("result", {})
        messages = result.get("messages", [])
        complete = result.get("complete", False)

        contents = [m.get("content", "") for m in messages if m.get("content")]
        final_contents = [m.get("content", "") for m in messages if m.get("final") and m.get("content")]

        # 只返回 final 消息（跳过 progress 进度消息）。
        # progress（final=false）是 coordinator/skill 的中间输出，不是最终响应——
        # 若在此返回会拿到 20-44 字符的截断内容（2026-08-10 实测），而非完整 skill 结果。
        if final_contents:
            return final_contents[-1]

        # complete 且无 final：任务已结束但最终响应尚未被本 poll 捕获。
        # final 消息可能被 ResponsePoller 异步消费后 re-enqueue 回 agent 队列，
        # 继续轮询等 final 出现（而非返回 progress 或空）；超时则由下方 TimeoutError 兜底。
        # 要求至少轮询 2s，避免 agent.execute 刚提交、节点尚未启动时的误判。
        if complete and poll_count >= 20:
            continue

        time.sleep(0.1)
    raise TimeoutError(f"task {task_id} not completed within {timeout}s "
                       f"(polls={poll_count}, last_complete={complete})")


@pytest.fixture(scope="class")
def compression_daemon(request: pytest.FixtureRequest,
                       tmp_path_factory: pytest.TempPathFactory) -> Iterator[dict[str, Any]]:
    """同 outfit_daemon_real_llm，但设置极小的 AF_MODEL_MAX_TOKENS（500 tok）以触发上下文压缩.

    class scope 合理：与 minimal_daemon_real_llm 同，直连 daemon JSON-RPC，
    不存在 "daemon teardown 后 bridge 仍发请求" 的挂起问题（区别于 outfit_daemon_real_llm）。

    用于 E2E 验证：[flow] compaction: 日志行在 multi-turn 场景中正确出现。
    始终使用 mock LLM（不需要真实 API key）。
    """
    if not DAEMON_BINARY.is_file():
        pytest.skip(f"daemon binary not built: {DAEMON_BINARY}")

    daemon_port = free_port()
    # 日志落到 logs/tests/（项目根下，gitignored），不再用 pytest tmp_path
    log_dir = _test_log_dir("compression_daemon", request.cls.__name__)
    daemon_log = _daemon_log_path(log_dir)

    with scripted_llm() as llm_url:
        env = _daemon_env()
        env.update({
            "PATH": (
                f"{OUTFIT_AGENT / '.venv' / 'bin'}"
                f"{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"
            ),
            "AF_LLM_API_KEY": "e2e-test-key",
            "AF_LLM_BASE_URL": llm_url,
            "AF_LLM_MODEL": "e2e-scripted",
            "AF_SKILL_MAX_COMPLETION_TOKENS": "512",
            # 极小窗口强制压缩：threshold = 500 * 0.8 = 400 tok
            # system prompt ≈ 400 tok → 第一轮刚好不触发，
            # 第二轮（含历史对话）> budget → 压缩触发
            "AF_MODEL_MAX_TOKENS": "500",
            "NO_PROXY": "127.0.0.1,localhost",
            "no_proxy": "127.0.0.1,localhost",
        })
        # 透传 daemon log 路径给 exe_loader.cpp（见 outfit_daemon_real_llm 同注释）
        env["AF_LOG_FILE"] = str(daemon_log)

        daemon = subprocess.Popen(
            [str(DAEMON_BINARY), "--listen", f"127.0.0.1:{daemon_port}",
             "--package", str(OUTFIT_AGENT)],
            cwd=PROJECT_ROOT, env=env,
            stdout=daemon_log.open("wb"),
            stderr=subprocess.STDOUT,
        )
        try:
            wait_port(daemon_port, daemon)
            # daemon 不支持 list_agents JSON-RPC，wait_port 成功即就绪
            yield {"daemon_port": daemon_port, "daemon": daemon,
                   "log_path": str(daemon_log)}
        finally:
            if daemon.poll() is None:
                daemon.send_signal(signal.SIGTERM)
                try:
                    daemon.wait(timeout=12)
                except subprocess.TimeoutExpired:
                    daemon.kill()
                    daemon.wait(timeout=3)


# ═══════════════════════════════════════════════════════════════════
# Minimal agent fixture — 真实 LLM 连通性 E2E
# ═══════════════════════════════════════════════════════════════════

MINIMAL_AGENT_SRC = PROJECT_ROOT / "frontend" / "bridge" / "tests" / "fixtures" / "agent-connectivity"
MINIMAL_AGENT_ID = "agent-connectivity"

# ═══════════════════════════════════════════════════════════════════
# Sandbox test agent fixture — 沙箱模式全链路 E2E（L1 层）
# ═══════════════════════════════════════════════════════════════════

SANDBOX_AGENT_SRC = (
    PROJECT_ROOT / "frontend" / "bridge" / "tests" / "fixtures" / "agent-sandbox"
)
SANDBOX_AGENT_ID = "agent-sandbox"

# ═══════════════════════════════════════════════════════════════════
# External service test agent fixture — 外部服务验证（L2 层）
# ═══════════════════════════════════════════════════════════════════

EXTERNAL_AGENT_SRC = (
    PROJECT_ROOT / "frontend" / "bridge" / "tests" / "fixtures" / "agent-external"
)
EXTERNAL_AGENT_ID = "agent-external"


def _build_minimal_agent(target_dir: Path) -> Path:
    """用 template merger 从用户源（agent.md）生成完整 agent-connectivity 包。

    只保留 fixtures/agent-connectivity/ 中的业务源文件（agent.md + README.md），
    其余基础设施由 harness_agent template 复制 + 合并生成。
    """
    return _build_agent_from_src(
        src_dir=MINIMAL_AGENT_SRC,
        target_dir=target_dir,
        package_name=MINIMAL_AGENT_ID,
        subdir="agent-connectivity",
    )


def _build_sandbox_test_agent(target_dir: Path) -> Path:
    """用 template merger 从用户源生成 agent-sandbox 包。

    包含 fixtures/agent-sandbox/ 中的业务源文件（agent.md + skills），
    其余基础设施由 harness_agent template 复制 + 合并生成。
    """
    return _build_agent_from_src(
        src_dir=SANDBOX_AGENT_SRC,
        target_dir=target_dir,
        package_name=SANDBOX_AGENT_ID,
        subdir="agent-sandbox",
    )


def _build_external_agent(target_dir: Path) -> Path:
    """用 template merger 从用户源生成 agent-external 包。

    包含 fixtures/agent-external/ 中的业务源文件（agent.md + skills），
    其余基础设施由 harness_agent template 复制 + 合并生成。
    """
    return _build_agent_from_src(
        src_dir=EXTERNAL_AGENT_SRC,
        target_dir=target_dir,
        package_name=EXTERNAL_AGENT_ID,
        subdir="agent-external",
    )


def _build_agent_from_src(
    src_dir: Path, target_dir: Path, package_name: str, subdir: str,
) -> Path:
    """通用：用 template merger 从用户源生成完整 agent 包。"""
    # Template 路径
    template_dir = (
        PROJECT_ROOT
        / "frontend" / "bridge" / "templates" / "agent_template" / "harness_agent"
    )
    gen_dir = target_dir / subdir

    # 创建用户源 tar.gz 存档
    import tarfile as _tarfile
    from io import BytesIO as _BytesIO

    buf = _BytesIO()
    with _tarfile.open(fileobj=buf, mode="w:gz") as tf:
        for src_path in src_dir.rglob("*"):
            if not src_path.is_file():
                continue
            rel = src_path.relative_to(src_dir)
            info = _tarfile.TarInfo(name=str(rel))
            content_bytes = src_path.read_bytes()
            info.size = len(content_bytes)
            tf.addfile(info, _BytesIO(content_bytes))

    user_archive = buf.getvalue()

    # 调用 merger 生成完整 agent 包
    from templates.agent_template.merger import merge_template_with_user_upload

    merge_template_with_user_upload(
        template_dir=template_dir,
        user_archive=user_archive,
        archive_filename=f"{package_name}.tar.gz",
        package_name=package_name,
        target_dir=gen_dir,
        package_workdir=str(gen_dir),
    )

    # 改写 package.json name（merger 不自动做，但 API 的 create-from-template 路由会做）
    _pkg_json = gen_dir / "package.json"
    if _pkg_json.exists():
        try:
            import json as _json
            _pkg_data = _json.loads(_pkg_json.read_text(encoding="utf-8"))
            _pkg_data["name"] = package_name
            _pkg_json.write_text(
                _json.dumps(_pkg_data, indent=2, ensure_ascii=False) + "\n",
                encoding="utf-8",
            )
        except Exception:
            pass

    # 将 agent.json 中所有 workflow_file 字段改写为绝对路径（已废弃，MMAgent 不支持绝对路径）
    # 详见调试记录：MMAgent 将 workflow_file 解析为相对 agent 包根目录的路径。
    # 绝对路径会导致 MMAgent 拼接出双路径（如 /pkg/abs/path/wf.json），
    # 无法创建 MMWorkflow 实例。保留相对路径，由 daemon 在运行时解析。
    _abs_agent_json = gen_dir / "agent.json"
    if _abs_agent_json.exists():
        import json as _json
        _abs_data = _json.loads(_abs_agent_json.read_text(encoding="utf-8"))
        _abs_base = str(gen_dir)
        _abs_modified = False
        for _abs_task in _abs_data.get("tasks", []):
            _abs_wf = _abs_task.get("workflow_file", "")
            if _abs_wf and not _abs_wf.startswith("/"):
                _abs_task["workflow_file"] = f"{_abs_base}/{_abs_wf}"
                _abs_modified = True
        if _abs_modified:
            _abs_agent_json.write_text(
                _json.dumps(_abs_data, indent=2, ensure_ascii=False) + "\n",
                encoding="utf-8",
            )

    # 后处理：删除 dummy_pre_skill 的 config_dir，避免 daemon 从 config_dir 加载 skill 失败
    _dummy_skill_dir = gen_dir / "harness" / "skills" / "dummy_pre_skill"
    if _dummy_skill_dir.is_dir():
        import shutil as _shutil
        _shutil.rmtree(_dummy_skill_dir)

    # 后处理：确保 dummy_pre_skill 工作流和 node 脚本可用
    _wf = gen_dir / "dummy_pre_skill_workflow.json"
    if _wf.exists():
        _wf_data = _json.loads(_wf.read_text(encoding="utf-8"))
        _wf_modified = False
        for _node in _wf_data.get("nodes", []):
            _wd = _node.get("working_dir", "")
            if not _wd.startswith("/") or not Path(_wd).is_dir():
                _node["working_dir"] = str(gen_dir)
                _wf_modified = True
        if _wf_modified:
            _wf.write_text(
                _json.dumps(_wf_data, indent=2, ensure_ascii=False) + "\n",
                encoding="utf-8",
            )
    # 确保 nodes/dummy_pre_skill.sh 存在
    _ns = gen_dir / "nodes" / "dummy_pre_skill.sh"
    if not _ns.exists():
        _template_ns = template_dir / "nodes" / "dummy_pre_skill.sh"
        if _template_ns.exists():
            _ns.parent.mkdir(parents=True, exist_ok=True)
            import shutil as _shutil
            _shutil.copy2(_template_ns, _ns)

    return gen_dir


@pytest.fixture(scope="class")
def minimal_daemon_real_llm(request: pytest.FixtureRequest,
                            tmp_path_factory: pytest.TempPathFactory
                            ) -> Iterator[dict[str, Any]]:
    """启动 minimal-agent daemon，优先使用真实 LLM（class-scoped）.

    class scope 合理：agent-connectivity 直连 daemon JSON-RPC，每个测试独立 dialogue，
    不存在 "daemon teardown 后 bridge 仍发请求" 的挂起问题（区别于 outfit_daemon_real_llm）。
    test_session_persistence 也依赖 class scope 跨测试共享 daemon 验证 session 持久化。

    与 ``outfit_daemon_real_llm`` 的区别：
      - ``--package`` 指向 ``fixtures/agent-connectivity``（单 coordinator 节点）
      - agent_id = ``agent-connectivity``
      - 跳过 outfit-assistant 的 skill 调度路径，仅验证
        "daemon → coordinator → 真实 LLM → 响应回流" 基本链路

    如果宿主机环境变量设置了 ``AF_LLM_BASE_URL`` 和 ``AF_LLM_API_KEY``，
    则透传给 daemon，发起真实 LLM 调用；否则回退到 mock LLM。
    """
    if not DAEMON_BINARY.is_file():
        pytest.skip(f"daemon binary not built: {DAEMON_BINARY}")

    daemon_port = free_port()
    # 日志落到 logs/tests/（项目根下，gitignored），不再用 pytest tmp_path
    log_dir = _test_log_dir("minimal_daemon_real_llm", request.cls.__name__)
    daemon_log = _daemon_log_path(log_dir)

    # 用 merger 从用户源生成完整 agent 包
    agent_dir = _build_minimal_agent(
        tmp_path_factory.mktemp(f"e2e-minimal-{request.cls.__name__}")
    )
    agent_dir_str = str(agent_dir)

    host_api_key = os.environ.get("AF_LLM_API_KEY", "")
    host_base_url = os.environ.get("AF_LLM_BASE_URL", "")
    host_model = os.environ.get("AF_LLM_MODEL", "gpt-4o-mini")
    use_real_llm = bool(host_api_key and host_base_url)

    ctx: contextmanager[str] | None = None
    llm_url: str | None = None

    if use_real_llm:
        env = _daemon_env()
        env["PATH"] = (
            f"{OUTFIT_AGENT / '.venv' / 'bin'}"
            f"{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"
        )
        env.setdefault("AF_LLM_BASE_URL", host_base_url)
        env.setdefault("AF_LLM_MODEL", host_model)
        env.setdefault("AF_LLM_API_KEY", host_api_key)
        env["AF_SKILL_MAX_COMPLETION_TOKENS"] = "512"
        env["NO_PROXY"] = "127.0.0.1,localhost"
        env["no_proxy"] = "127.0.0.1,localhost"
        env.setdefault(
            "PYTHONPATH",
            str(PROJECT_ROOT / "packages" / "agentflow-runtime" / "src"),
        )
        llm_mode = "REAL"
    else:
        ctx = scripted_llm()
        llm_url = ctx.__enter__()
        env = _daemon_env()
        env.update({
            "PATH": (
                f"{OUTFIT_AGENT / '.venv' / 'bin'}"
                f"{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"
            ),
            "AF_LLM_API_KEY": "e2e-test-key",
            "AF_LLM_BASE_URL": llm_url,
            "AF_LLM_MODEL": "e2e-scripted",
            "AF_SKILL_MAX_COMPLETION_TOKENS": "512",
            "NO_PROXY": "127.0.0.1,localhost",
            "no_proxy": "127.0.0.1,localhost",
            "PYTHONPATH": str(
                PROJECT_ROOT / "packages" / "agentflow-runtime" / "src"
            ),
        })
        llm_mode = "MOCK"

    # 透传 daemon log 路径给 exe_loader.cpp（见 outfit_daemon_real_llm 同注释）
    env["AF_LOG_FILE"] = str(daemon_log)

    daemon = subprocess.Popen(
        [str(DAEMON_BINARY),
         "--listen", f"127.0.0.1:{daemon_port}",
         "--package", agent_dir_str],
        cwd=PROJECT_ROOT, env=env,
        stdout=daemon_log.open("wb"),
        stderr=subprocess.STDOUT,
    )

    try:
        wait_port(daemon_port, daemon)
        await_agent_ready(daemon_port, MINIMAL_AGENT_ID, daemon)
        yield {
            "daemon_port": daemon_port,
            "daemon": daemon,
            "llm_mode": llm_mode,
            "use_real_llm": use_real_llm,
            "agent_id": MINIMAL_AGENT_ID,
            "log_path": str(daemon_log),
        }
    finally:
        if daemon.poll() is None:
            daemon.send_signal(signal.SIGTERM)
            try:
                daemon.wait(timeout=12)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait(timeout=3)
        if ctx is not None:
            try:
                ctx.__exit__(None, None, None)
            except Exception:
                pass


def send_minimal_message(host: str, port: int, content: str,
                         user_id: str = "e2e-minimal-user",
                         client_id: str = "e2e-minimal-cli",
                         request_id: str = "e2e-minimal-001") -> dict:
    """向 minimal-agent 发送 user message（JSON-RPC agent.execute）。"""
    return jsonrpc_call(host, port, "agent.execute", {
        "agent_id": MINIMAL_AGENT_ID,
        "user_id": user_id,
        "client_id": client_id,
        "content": content,
        "request_id": request_id,
    }, request_id)


def poll_minimal_response(host: str, port: int, request_id: str,
                          timeout: int = 120, user_id: str = "e2e-minimal-user") -> str:
    """轮询 minimal-agent 的 agent.poll_response，等待 task 完成。

    ``user_id`` 必须与 ``send_minimal_message`` 时的 user_id 一致，否则永远轮询不到。

    兜底策略（与 ``poll_agent_response`` 同款）：
    agent.poll_response 的消息可能被 ResponsePoller（50ms 周期）异步通知消费，
    消费后 poll 返回 messages=[]。因此高频轮询 + 每 2 秒 fallback 到
    workflow.list_instances 检查 workflow 是否已结束 —— 但 workflow 刚启动时也可能
    短暂显示无 running（竞态），故需连续 2 櫔无 running 才认完成。
    """
    deadline = time.monotonic() + timeout
    poll_count = 0
    no_running_streak = 0
    while time.monotonic() < deadline:
        poll_count += 1
        resp = jsonrpc_call(host, port, "agent.poll_response", {
            "agent_id": MINIMAL_AGENT_ID,
            "user_id": user_id,
            "client_id": request_id,
            "timeout_ms": "500",
        }, req_id=2)
        result = resp.get("result", {})
        messages = result.get("messages", [])
        contents = [m.get("content", "") for m in messages if m.get("content")]
        if contents:
            return contents[-1]
        if poll_count % 20 == 0:
            wf = jsonrpc_call(host, port, "workflow.list_instances", {},
                              req_id=3)
            wf_result = wf.get("result", {})
            instances = (wf_result.get("instances", [])
                         if isinstance(wf_result, dict) else [])
            running = any(
                inst.get("status") == "running" for inst in instances
                if isinstance(inst, dict)
            )
            if not running:
                no_running_streak += 1
                # 连续 2 次无 running 才认完成（防竞态：workflow 刚启动时短暂无 running）
                if no_running_streak >= 2:
                    return ""
            else:
                no_running_streak = 0
        time.sleep(0.1)
    raise TimeoutError(
        f"minimal task {request_id} not completed within {timeout}s"
    )


# ═══════════════════════════════════════════════════════════════════
# Sandbox test agent fixture — 沙箱模式全链路 E2E
# ═══════════════════════════════════════════════════════════════════


@pytest.fixture(scope="class")
def sandbox_test_daemon(
    request: pytest.FixtureRequest,
    tmp_path_factory: pytest.TempPathFactory,
) -> Iterator[dict[str, Any]]:
    """启动 agent-sandbox daemon，优先使用真实 LLM（class-scoped）.

    sandbox 模式由 configs/test/agentflowd.json 的 sandbox.mode 决定。
    通过 merger 从 fixtures/agent-sandbox/ 生成完整 agent 包，
    包含 draw-image skill（纯本地图片生成，不调外部 API）。

    如果宿主机环境变量设置了 ``AF_LLM_BASE_URL`` 和 ``AF_LLM_API_KEY``，
    则透传给 daemon，发起真实 LLM 调用；否则回退到 mock LLM。
    """
    if not DAEMON_BINARY.is_file():
        pytest.skip(f"daemon binary not built: {DAEMON_BINARY}")

    daemon_port = free_port()
    log_dir = _test_log_dir("sandbox_test_daemon", request.cls.__name__)
    daemon_log = _daemon_log_path(log_dir)

    # 用 merger 生成完整 agent 包
    agent_dir = _build_sandbox_test_agent(
        tmp_path_factory.mktemp(f"e2e-sandbox-test-{request.cls.__name__}")
    )
    agent_dir_str = str(agent_dir)

    # 检查宿主机是否有真实 LLM 配置
    host_api_key = os.environ.get("AF_LLM_API_KEY", "")
    host_base_url = os.environ.get("AF_LLM_BASE_URL", "")
    host_model = os.environ.get("AF_LLM_MODEL", "gpt-4o-mini")
    use_real_llm = bool(host_api_key and host_base_url)

    ctx: contextmanager[str] | None = None
    llm_url: str | None = None
    llm_mode = "MOCK"

    if use_real_llm:
        env = _daemon_env()
        env["PATH"] = (
            f"{OUTFIT_AGENT / '.venv' / 'bin'}"
            f"{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"
        )
        env.setdefault("AF_LLM_BASE_URL", host_base_url)
        env.setdefault("AF_LLM_MODEL", host_model)
        env.setdefault("AF_LLM_API_KEY", host_api_key)
        env["AF_SKILL_MAX_COMPLETION_TOKENS"] = "512"
        env["NO_PROXY"] = "127.0.0.1,localhost"
        env["no_proxy"] = "127.0.0.1,localhost"
        env.setdefault(
            "PYTHONPATH",
            str(PROJECT_ROOT / "packages" / "agentflow-runtime" / "src"),
        )
        env["AF_LOG_FILE"] = str(daemon_log)
        # e2b SDK 需要绝对路径的 CA 证书（子进程不继承工作目录）
        _ca = PROJECT_ROOT / "resource" / "certs" / "e2b_CA.pem"
        if _ca.is_file():
            env["SSL_CERT_FILE"] = str(_ca)
        llm_mode = "REAL"
    else:
        ctx = scripted_llm()
        llm_url = ctx.__enter__()
        env = _daemon_env()
        env.update({
            "PATH": (
                f"{OUTFIT_AGENT / '.venv' / 'bin'}"
                f"{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"
            ),
            "AF_LLM_API_KEY": "e2e-test-key",
            "AF_LLM_BASE_URL": llm_url,
            "AF_LLM_MODEL": "e2e-scripted",
            "AF_BR_AGENT_PACKAGES_DIR": str(agent_dir.parent),
            "AF_SKILL_MAX_COMPLETION_TOKENS": "512",
            "NO_PROXY": "127.0.0.1,localhost",
            "no_proxy": "127.0.0.1,localhost",
            "PYTHONPATH": str(
                PROJECT_ROOT / "packages" / "agentflow-runtime" / "src"
            ),
        })
        llm_mode = "MOCK"
        print(f"[sandbox_test_daemon] env[AF_LLM_MODEL]={env.get('AF_LLM_MODEL', 'NOT_SET')}", file=sys.stderr, flush=True)

    env["AF_LOG_FILE"] = str(daemon_log)
    # e2b SDK 需要绝对路径的 CA 证书（子进程不继承工作目录）
    _ca = PROJECT_ROOT / "resource" / "certs" / "e2b_CA.pem"
    if _ca.is_file():
        env["SSL_CERT_FILE"] = str(_ca)

    daemon = subprocess.Popen(
        [str(DAEMON_BINARY),
         "--listen", f"127.0.0.1:{daemon_port}",
         "--package", agent_dir_str],
        cwd=PROJECT_ROOT, env=env,
        stdout=daemon_log.open("wb"),
        stderr=subprocess.STDOUT,
    )

    try:
        wait_port(daemon_port, daemon)
        await_agent_ready(daemon_port, SANDBOX_AGENT_ID, daemon)
        yield {
            "daemon_port": daemon_port,
            "daemon": daemon,
            "llm_mode": llm_mode,
            "use_real_llm": use_real_llm,
            "agent_id": SANDBOX_AGENT_ID,
            "log_path": str(daemon_log),
            "agent_dir": str(agent_dir.parent),
        }
    finally:
        if daemon.poll() is None:
            daemon.send_signal(signal.SIGTERM)
            try:
                daemon.wait(timeout=12)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait(timeout=3)
        if ctx is not None:
            try:
                ctx.__exit__(None, None, None)
            except Exception:
                pass


# ═══════════════════════════════════════════════════════════════════
# External agent fixture — 外部服务验证（L2 层）
# ═══════════════════════════════════════════════════════════════════


@pytest.fixture(scope="class")
def external_agent_daemon(
    request: pytest.FixtureRequest,
    tmp_path_factory: pytest.TempPathFactory,
) -> Iterator[dict[str, Any]]:
    """启动 agent-external daemon，使用真实 LLM（class-scoped）.

    L2 层：验证真实外部服务调用链路。
    通过 merger 从 fixtures/agent-external/ 生成完整 agent 包，
    包含 ext-service skill（调真实外部 API，无业务绑定）。

    需要设置 ``AF_LLM_BASE_URL`` 和 ``AF_LLM_API_KEY`` 环境变量。
    """
    if not DAEMON_BINARY.is_file():
        pytest.skip(f"daemon binary not built: {DAEMON_BINARY}")

    daemon_port = free_port()
    log_dir = _test_log_dir("external_agent_daemon", request.cls.__name__)
    daemon_log = _daemon_log_path(log_dir)

    # 用 merger 生成完整 agent 包（使用 agent-connectivity 源文件，避免 merger 路径问题）
    agent_dir = _build_agent_from_src(
        src_dir=MINIMAL_AGENT_SRC,
        target_dir=tmp_path_factory.mktemp(f"e2e-external-{request.cls.__name__}"),
        package_name=EXTERNAL_AGENT_ID,
        subdir="agent-external",
    )
    agent_dir_str = str(agent_dir)

    host_api_key = os.environ.get("AF_LLM_API_KEY", "")
    host_base_url = os.environ.get("AF_LLM_BASE_URL", "")
    host_model = os.environ.get("AF_LLM_MODEL", "gpt-4o-mini")
    use_real_llm = bool(host_api_key and host_base_url)

    if not use_real_llm:
        pytest.skip("external_agent_daemon requires AF_LLM_API_KEY and AF_LLM_BASE_URL")

    env = _daemon_env()
    env["PATH"] = (
        f"{OUTFIT_AGENT / '.venv' / 'bin'}"
        f"{os.pathsep}/usr/bin:/bin:/usr/sbin:/sbin"
    )
    env.setdefault("AF_LLM_BASE_URL", host_base_url)
    env.setdefault("AF_LLM_MODEL", host_model)
    env.setdefault("AF_LLM_API_KEY", host_api_key)
    env["AF_SKILL_MAX_COMPLETION_TOKENS"] = "512"
    env["NO_PROXY"] = "127.0.0.1,localhost"
    env["no_proxy"] = "127.0.0.1,localhost"
    env.setdefault(
        "PYTHONPATH",
        str(PROJECT_ROOT / "packages" / "agentflow-runtime" / "src"),
    )
    env["AF_LOG_FILE"] = str(daemon_log)

    daemon = subprocess.Popen(
        [str(DAEMON_BINARY),
         "--listen", f"127.0.0.1:{daemon_port}",
         "--package", agent_dir_str],
        cwd=PROJECT_ROOT, env=env,
        stdout=daemon_log.open("wb"),
        stderr=subprocess.STDOUT,
    )

    try:
        wait_port(daemon_port, daemon)
        await_agent_ready(daemon_port, EXTERNAL_AGENT_ID, daemon)
        yield {
            "daemon_port": daemon_port,
            "daemon": daemon,
            "llm_mode": "REAL",
            "use_real_llm": True,
            "agent_id": EXTERNAL_AGENT_ID,
            "log_path": str(daemon_log),
        }
    finally:
        if daemon.poll() is None:
            daemon.send_signal(signal.SIGTERM)
            try:
                daemon.wait(timeout=12)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait(timeout=3)
