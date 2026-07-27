"""跨模式一致性测试 — 验证 LocalSandbox 与 DockerSandbox 行为完全一致（B18）。

需要 Docker daemon 可用，同时依赖 local_sandbox 和 docker_sandbox 两个 fixture。
环境变量 DOCKER_SANDBOX_ENABLED=1 时启用。
"""

import pytest
import json

from .sandbox_client import SandboxClient


def _normalize_result(result: dict) -> dict:
    """归一化结果，去除运行时差异字段（duration_ms 等），只保留行为关键字段。"""
    return {
        "exit_code": result.get("exit_code"),
        "stdout": result.get("stdout", "").strip(),
        "stderr": result.get("stderr", "").strip(),
        "truncated": result.get("truncated", False),
    }


class TestModeConsistency:
    """LocalSandbox ↔ DockerSandbox 行为一致性验证。"""

    # M1: echo 返回格式一致
    def test_echo_consistent(self, local_sandbox: SandboxClient,
                             docker_sandbox: SandboxClient) -> None:
        """两个 mode 的 echo 返回 JSON 结构完全相同。"""
        local_result = local_sandbox.bash("echo hello_consistency")
        docker_result = docker_sandbox.bash("echo hello_consistency")

        local_norm = _normalize_result(local_result)
        docker_norm = _normalize_result(docker_result)

        assert local_norm == docker_norm, (
            f"Local/Docker 行为不一致\n"
            f"  local:  {json.dumps(local_norm, ensure_ascii=False)}\n"
            f"  docker: {json.dumps(docker_norm, ensure_ascii=False)}"
        )

    # M2: 危险命令拒绝一致
    def test_dangerous_rejected_consistent(self, local_sandbox: SandboxClient,
                                           docker_sandbox: SandboxClient) -> None:
        """两个 mode 的危险命令拒绝行为一致。"""
        local_result = local_sandbox.bash("rm -rf /")
        docker_result = docker_sandbox.bash("rm -rf /")

        local_norm = _normalize_result(local_result)
        docker_norm = _normalize_result(docker_result)

        # 两个 mode 都应拒绝
        assert local_norm["exit_code"] != 0
        assert docker_norm["exit_code"] != 0
        # 错误信息都应包含 "Dangerous"
        assert "Dangerous" in (local_norm["stderr"] or "")
        assert "Dangerous" in (docker_norm["stderr"] or "")

    # M3: 截断行为一致
    def test_truncation_consistent(self, local_sandbox: SandboxClient,
                                   docker_sandbox: SandboxClient) -> None:
        """两个 mode 的截断行为一致。"""
        local_result = local_sandbox.bash(
            "python3 -c \"print('x'*5000)\"",
            opts={"output_max_bytes": 100},
        )
        docker_result = docker_sandbox.bash(
            "python3 -c \"print('x'*5000)\"",
            opts={"output_max_bytes": 100},
        )

        local_norm = _normalize_result(local_result)
        docker_norm = _normalize_result(docker_result)

        # 两个 mode 都应截断
        assert local_norm["truncated"] is True
        assert docker_norm["truncated"] is True
        # 截断后长度都应 ≤100
        assert len(local_norm["stdout"].encode("utf-8")) <= 100
        assert len(docker_norm["stdout"].encode("utf-8")) <= 100

    # M4: 路径拒绝一致
    def test_path_escape_consistent(self, local_sandbox: SandboxClient,
                                    docker_sandbox: SandboxClient) -> None:
        """两个 mode 的路径拒绝行为一致。"""
        local_result = local_sandbox.read_file(
            "../etc/passwd",
            opts={"working_dir": "/tmp"},
        )
        docker_result = docker_sandbox.read_file(
            "../etc/passwd",
            opts={"working_dir": "/tmp"},
        )

        # 两个 mode 都应拒绝
        assert local_result.get("exit_code") != 0, (
            f"local 应拒绝越界路径: {local_result}"
        )
        assert docker_result.get("exit_code") != 0, (
            f"docker 应拒绝越界路径: {docker_result}"
        )

    # M5: 未知 tool 拒绝一致
    def test_unknown_tool_consistent(self, local_sandbox: SandboxClient,
                                     docker_sandbox: SandboxClient) -> None:
        """两个 mode 的未知 tool 拒绝行为一致。"""
        local_result = local_sandbox.exec("nonexistent_tool", {})
        docker_result = docker_sandbox.exec("nonexistent_tool", {})

        local_norm = _normalize_result(local_result)
        docker_norm = _normalize_result(docker_result)

        # 两个 mode 都应拒绝
        assert local_norm["exit_code"] != 0
        assert docker_norm["exit_code"] != 0
        # 错误信息都应包含 Unknown tool
        assert "Unknown tool" in (local_norm["stderr"] or "")
        assert "Unknown tool" in (docker_norm["stderr"] or "")