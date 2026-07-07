"""Daemon lifecycle manager for e2e tests.

Each test gets a fresh daemon via setup_method to avoid shared memory
degradation after ~5 requests.
"""
import os
import signal
import subprocess
import time

_DAEMON_PID: int | None = None
_DAEMON_LOG: str = "/tmp/af_e2e_daemon.log"


def start(port: int = 17891, max_retries: int = 3) -> str:
    """Start a fresh daemon, return the log file path.

    Retries up to ``max_retries`` times if daemon crashes on startup
    (shared memory mutex from previous unclean shutdown).
    """
    global _DAEMON_PID

    project_root = os.path.join(os.path.dirname(__file__), "..", "..", "..")
    package = os.path.join(project_root, "examples", "agent", "outfit-assistant")
    venv = os.path.join(package, ".venv")
    log_path = f"/tmp/af_e2e_t{int(time.time())}.log"

    for attempt in range(1, max_retries + 1):
        # Clean DataBus artifacts
        for pattern in ["/tmp/agentflow_*",
                        "/tmp/af_e2e_*.log",
                        "/var/folders/bt/2yw82p395lzgl06m3vlg2g7w0000gn/T/agentflow_databus/"]:
            subprocess.run(f"rm -rf {pattern}", shell=True, capture_output=True)
        time.sleep(2)

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
                import socket, json
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
