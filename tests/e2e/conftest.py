"""E2E 沙箱测试共享 fixture。

提供 local_sandbox 和 docker_sandbox 两个 fixture，均返回 SandboxClient 实例。
测试代码通过 SandboxClient 访问沙箱，不感知 mode 差异。
"""

import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import pytest

from .sandbox_client import SandboxClient

# ── 路径常量 ──────────────────────────────────────────────────────────

_PROJECT_ROOT = Path(__file__).resolve().parents[2]
_SANDBOX_SERVER = str(
    _PROJECT_ROOT / "docker" / "sandbox" / "http_server" / "sandbox_http_server.py"
)
_DOCKERFILE = str(_PROJECT_ROOT / "docker" / "sandbox" / "Dockerfile")
_DOCKER_IMAGE_TAG = "agentflow-sandbox:v1"
_DOCKER_SANDBOX_PORT = 8080
# 沙箱只需 harness/skills/（供 list_skills/load_skill 读取 SKILL.md），
# harness/tools/ 在 daemon 侧 agentflow-runtime 包中，不需要进沙箱。
# 复制到 workdir/harness/skills/（与生产 fs::copy 行为一致），
# 通过 SANDBOX_WORKDIR=/workdir 让 HTTP server 找到。
_SKILLS_SRC_DIR = str(_PROJECT_ROOT / "examples" / "agent" / "outfit-assistant" / "harness" / "skills")

# 是否跳过 Docker 测试（无 Docker daemon 时）
_SKIP_DOCKER = not os.environ.get("DOCKER_SANDBOX_ENABLED", "")


# ── 辅助函数 ──────────────────────────────────────────────────────────


def _find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _wait_ready(endpoint: str, proc: subprocess.Popen[bytes] | None = None,
                timeout: int = 15) -> None:
    """等待沙箱 HTTP server 就绪。"""
    import requests
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            resp = requests.post(
                f"{endpoint}/exec",
                json={
                    "tool": "bash",
                    "tool_params": {"command": "echo ready"},
                    "opts": {},
                },
                timeout=2,
            )
            if resp.status_code == 200:
                return
        except Exception:
            if proc and proc.poll() is not None:
                stderr = proc.stderr.read().decode() if proc.stderr else ""
                pytest.fail(f"sandbox server exited: {stderr}")
            time.sleep(0.2)
    pytest.fail(f"sandbox did not become ready at {endpoint} within {timeout}s")


# ── LocalSandbox fixture ──────────────────────────────────────────────


@pytest.fixture(scope="module")
def local_sandbox():
    """启动本地 sandbox HTTP server 子进程，返回 SandboxClient。

    使用 Popen 启动 sandbox_http_server.py，自动分配端口。
    测试结束后 SIGTERM 终止子进程。
    """
    port = _find_free_port()
    proc = subprocess.Popen(
        [sys.executable, _SANDBOX_SERVER, "--port", str(port), "--host", "127.0.0.1"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=str(Path(_SANDBOX_SERVER).parent),
    )
    endpoint = f"http://127.0.0.1:{port}"
    _wait_ready(endpoint, proc)
    client = SandboxClient(endpoint)
    yield client
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)
    finally:
        # 清理 server CWD 中测试可能残留的 .txt 文件
        _cleanup_server_dir()
        proc.stdout.close()
        proc.stderr.close()


def _cleanup_server_dir() -> None:
    """清理 sandbox HTTP server CWD 中可能残留的测试文件。"""
    server_dir = Path(_SANDBOX_SERVER).parent
    for f in server_dir.glob("*.txt"):
        try:
            f.unlink()
        except OSError:
            pass


# ── DockerSandbox fixture ─────────────────────────────────────────────


@pytest.fixture(scope="module")
def docker_sandbox():
    """构建并启动 Docker 沙箱容器，返回 SandboxClient。

    需要 Docker daemon 可用 + 镜像已构建。
    测试结束后 stop + remove 容器。
    环境变量 DOCKER_SANDBOX_ENABLED=1 时启用。
    """
    if _SKIP_DOCKER:
        pytest.skip("Docker 沙箱测试需设置 DOCKER_SANDBOX_ENABLED=1")

    _build_docker_image()
    # 创建宿主机 workdir 目录，挂载到容器 /workdir
    _workdir_host = tempfile.mkdtemp(prefix="sandbox_workdir_")
    print(f"[docker] host workdir: {_workdir_host}")
    # 复制 skills 到 workdir/harness/skills/（与生产 fs::copy 行为一致）
    _skills_dst = Path(_workdir_host) / "harness" / "skills"
    _skills_dst.mkdir(parents=True, exist_ok=True)
    for skill_dir in Path(_SKILLS_SRC_DIR).iterdir():
        if skill_dir.is_dir():
            _target = _skills_dst / skill_dir.name
            shutil.copytree(skill_dir, _target, dirs_exist_ok=True)
    print(f"[docker] skills synced to workdir: {_skills_dst}")
    try:
        client = _run_docker_container(
            workdir_host_path=_workdir_host,
        )
    except BaseException:
        # 容器启动失败时清理 workdir 目录，避免资源泄漏
        shutil.rmtree(_workdir_host, ignore_errors=True)
        print(f"[docker] cleaned workdir after startup failure: {_workdir_host}")
        raise
    yield client
    # 清理容器
    _cleanup_docker_container(client)
    # 清理宿主机 workdir 目录
    shutil.rmtree(_workdir_host, ignore_errors=True)
    print(f"[docker] cleaned host workdir: {_workdir_host}")


def _build_docker_image():
    """构建沙箱 Docker 镜像（如果不存在）。

    注意：需临时清除 HTTP_PROXY 环境变量，否则 docker-py 会通过代理
    连接本地 Unix socket 导致 ConnectionRefused。
    Docker daemon 不可用时抛出跳过（skip）异常。
    """
    import docker  # docker-py
    # 清除代理环境变量，防止 docker-py 通过代理连接本地 Unix socket
    _saved_env = {}
    for _var in ("HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy"):
        _saved_env[_var] = os.environ.pop(_var, None)
    try:
        client = docker.from_env()
    except Exception as e:
        pytest.skip(f"Docker daemon 不可达: {e}")
    try:
        # 验证 Docker daemon 可达
        client.ping()
        try:
            client.images.get(_DOCKER_IMAGE_TAG)
            print(f"[docker] image {_DOCKER_IMAGE_TAG} already exists")
        except docker.errors.ImageNotFound:
            print(f"[docker] building image {_DOCKER_IMAGE_TAG}...")
            try:
                image, _ = client.images.build(
                    path=str(_PROJECT_ROOT),
                    dockerfile=_DOCKERFILE,
                    tag=_DOCKER_IMAGE_TAG,
                )
                print(f"[docker] built: {image.short_id}")
            except docker.errors.BuildError as e:
                pytest.skip(f"Docker 镜像构建失败（网络/镜像拉取问题）: {e}")
    finally:
        # 恢复代理环境变量
        for _var, _val in _saved_env.items():
            if _val is not None:
                os.environ[_var] = _val


def _run_docker_container(workdir_host_path: str | None = None):
    """运行沙箱容器，自动分配宿主机端口。

    先清理残留的旧容器，再使用随机可用端口启动新容器。
    workdir 挂载到容器内 /workdir，skills 已预复制到 workdir/harness/skills/，
    设置 SANDBOX_WORKDIR=/workdir 使 HTTP server 能找到 skills。
    """
    import docker
    import socket
    client = docker.from_env()

    # 清理残留的旧容器（同名镜像的容器可能未清理）
    _cleanup_old_containers(client)

    # 容器内固定端口 8080（Dockerfile ENTRYPOINT）
    _container_port = 8080
    # 自动分配宿主机端口
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        _host_port = s.getsockname()[1]

    # 构建 volumes 映射
    _volumes = {
        str(_PROJECT_ROOT / "docker" / "sandbox" / "http_server"): {
            "bind": "/opt/sandbox/http_server",
            "mode": "ro",
        },
    }
    # 可选：宿主机 workdir 挂载到容器内 /workdir
    if workdir_host_path:
        _volumes[workdir_host_path] = {
            "bind": "/workdir",
            "mode": "rw",
        }
    # skills 已预复制到 workdir/harness/skills/，SANDBOX_WORKDIR=/workdir 让 HTTP server 找到
    _environment = {"AF_SANDBOX_WORKSPACE": "/workdir"}

    try:
        container = client.containers.run(
            _DOCKER_IMAGE_TAG,
            detach=True,
            ports={_container_port: _host_port},
            remove=True,
            # 宿主机目录映射：http_server(ro) + workdir(rw)
            volumes=_volumes,
            environment=_environment,
        )
        print(f"[docker] container started: {container.short_id} "
              f"host_port={_host_port} -> container_port={_container_port}")
        # 返回 endpoint（使用宿主机端口）
        _endpoint = f"http://127.0.0.1:{_host_port}"
        # 等待容器就绪（Docker 容器启动比本地进程慢，给 30s 超时）
        _wait_ready(_endpoint, timeout=30)
        from .sandbox_client import SandboxClient
        return SandboxClient(_endpoint)
    except docker.errors.APIError as e:
        pytest.skip(f"Docker 容器启动失败: {e}")


def _cleanup_old_containers(client):
    """清理残留的旧沙箱容器。"""
    try:
        containers = client.containers.list(
            all=True,
            filters={"ancestor": _DOCKER_IMAGE_TAG},
        )
        for c in containers:
            try:
                print(f"  [docker] 清理残留容器: {c.short_id}")
                c.stop(timeout=5)
                c.remove(force=True)
            except Exception:
                pass
    except Exception:
        pass


def _cleanup_docker_container(client):
    """清理 Docker 沙箱容器（fixture teardown 时调用）。"""
    try:
        containers = client.containers.list(
            all=True,
            filters={"ancestor": _DOCKER_IMAGE_TAG},
        )
        for c in containers:
            try:
                print(f"  [docker] 清理容器: {c.short_id}")
                c.stop(timeout=10)
                c.remove(force=True)
            except Exception:
                pass
    except Exception:
        pass