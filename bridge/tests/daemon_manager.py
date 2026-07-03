"""Daemon lifecycle manager for e2e tests.

Each test gets a fresh daemon via setup_method to avoid shared memory
degradation after ~5 requests.
"""
import os
import signal
import socket
import subprocess
import sys
import time

_DAEMON_PID: int | None = None
_DAEMON_LOG: str = "/tmp/af_e2e_daemon.log"


def _wait_port_free(port: int, timeout: int = 60) -> bool:
    """Wait for port to be usable for bind().

    Uses ``bind()`` instead of ``connect()`` because:
    - ``connect()`` returns ConnectionRefusedError during TIME_WAIT
      even though ``bind()`` would still get EADDRINUSE.
    - An actual ``bind()`` probe is the only reliable check.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            if hasattr(socket, "SO_REUSEPORT"):
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
            s.settimeout(1)
            s.bind(("127.0.0.1", port))
            s.close()
            return True
        except OSError:
            time.sleep(2)
            continue
    return False


def _kill_on_port(port: int) -> None:
    """Kill any process listening on ``port`` (macOS ``lsof -ti``)."""
    try:
        result = subprocess.run(
            ["lsof", "-ti", f"tcp:{port}"],
            capture_output=True, text=True, timeout=5,
        )
        for pid in result.stdout.strip().splitlines():
            try:
                os.kill(int(pid), signal.SIGTERM)
            except (OSError, ProcessLookupError, ValueError):
                pass
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass
    time.sleep(1)


def start(port: int = 17891, max_retries: int = 3) -> str:
    """Start a fresh daemon, return the log file path.

    Retries up to ``max_retries`` times if daemon crashes on startup
    (shared memory mutex from previous unclean shutdown).
    """
    global _DAEMON_PID

    # Kill any existing daemon on the target port
    _kill_on_port(port)

    project_root = os.path.join(os.path.dirname(__file__), "..", "..")
    package = os.path.join(project_root, "examples", "agent", "outfit_assistant")
    venv = os.path.join(package, ".venv")
    log_path = f"/tmp/af_e2e_t{int(time.time())}.log"

    for attempt in range(1, max_retries + 1):
        # Clean DataBus artifacts
        for pattern in ["/tmp/agentflow_*",
                        "/tmp/af_e2e_*.log",
                        "/var/folders/bt/2yw82p395lzgl06m3vlg2g7w0000gn/T/agentflow_databus/"]:
            subprocess.run(f"rm -rf {pattern}", shell=True, capture_output=True)
        time.sleep(2)

        # Wait for port to be free — abort this attempt if timeout
        if not _wait_port_free(port, timeout=30):
            print(f"[daemon] port {port} still in use after 30s, retry {attempt}/{max_retries}",
                  file=sys.stderr)
            continue

        env = dict(os.environ, PYTHON_PATH=venv)
        proc = subprocess.Popen(
            ["./build/agentflowd", "--listen", f"127.0.0.1:{port}",
             "--package", package],
            cwd=project_root, env=env,
            stdout=open(log_path, "w"), stderr=subprocess.STDOUT,
        )

        # Wait for daemon to be ready
        for _ in range(25):
            time.sleep(1)
            if proc.poll() is not None:
                break  # daemon died, will retry
            try:
                import json
                s = socket.socket(); s.settimeout(2)
                s.connect(("127.0.0.1", port))
                s.sendall((json.dumps({"jsonrpc": "2.0",
                    "method": "agent.list", "params": {}, "id": 1}) + "\n").encode())
                resp_data = s.recv(4096).decode()
                s.close()
                if "outfit-assistant" in resp_data:
                    _DAEMON_PID = proc.pid
                    _DAEMON_LOG = log_path
                    return log_path
            except Exception:
                continue
        else:
            # Timeout waiting for daemon
            proc.kill()
            proc.wait()
            continue  # retry

    raise RuntimeError(
        f"daemon not ready after {max_retries} attempts. "
        f"Check latest log: {log_path}"
    )


def stop():
    """Gracefully stop the daemon."""
    global _DAEMON_PID
    if _DAEMON_PID:
        try:
            os.kill(_DAEMON_PID, signal.SIGTERM)
            time.sleep(3)
            os.kill(_DAEMON_PID, signal.SIGKILL)
        except (OSError, ProcessLookupError):
            pass
        _DAEMON_PID = None


def log_path() -> str:
    return _DAEMON_LOG or "/tmp/af_e2e_daemon.log"
