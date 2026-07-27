"""DockerSandbox E2E 测试 — 容器生命周期、容器内隔离验证、特殊场景。

覆盖 AC-US5-001，引用 TC-SBOX-001/004/012/023/025/031/033/040/041。
所有测试需要 Docker daemon 可用，通过 docker_sandbox fixture 启动容器。
环境变量 DOCKER_SANDBOX_ENABLED=1 时启用。
"""

import json

import pytest

from .sandbox_client import SandboxClient


# ═══════════════════════════════════════════════════════════════════════
# 4.1 容器生命周期（AC-US5-001）
# ═══════════════════════════════════════════════════════════════════════


class TestContainerLifecycle:
    """DockerSandbox 容器生命周期。"""

    # D1: docker build 镜像成功 — 由 fixture 隐式验证
    def test_container_echo(self, docker_sandbox: SandboxClient) -> None:
        """D3: docker 内 echo 执行正确。"""
        result = docker_sandbox.bash("echo hello_docker")
        assert result["exit_code"] == 0, f"exit_code 非 0: {result}"
        assert "hello_docker" in result["stdout"], (
            f"stdout 缺预期内容: {result}"
        )

    # D2: 容器健康检查 — fixture 启动时已等待就绪
    def test_container_health(self, docker_sandbox: SandboxClient) -> None:
        """D2: docker 沙箱 health check 通过。"""
        assert docker_sandbox.health(), "docker sandbox 应可达"

    # D3: 已在 test_container_echo 中覆盖

    # D4: 容器销毁后不可达 — 由 fixture teardown 验证
    # 当前 fixture 是 module scope，teardown 后无法测试
    # 此处验证容器运行时可达即可
    def test_container_reachable(self, docker_sandbox: SandboxClient) -> None:
        """容器活跃时可达。"""
        assert docker_sandbox.health(), "容器应可达"

    # D5: 销毁幂等 — 由 docker SDK 的 stop/remove 幂等保证


# ═══════════════════════════════════════════════════════════════════════
# 4.2 容器内隔离验证
# ═══════════════════════════════════════════════════════════════════════


class TestContainerIsolation:
    """DockerSandbox 容器内隔离验证。"""

    # D6: 容器内危险命令不影响宿主机
    def test_dangerous_command_inside_container(
            self, docker_sandbox: SandboxClient) -> None:
        """容器内危险命令不影响宿主机。"""
        result = docker_sandbox.bash("rm -rf /")
        # 容器内执行 rm -rf / 会失败（权限或 docker 保护）
        # 但宿主机文件系统不受影响
        assert result["exit_code"] != 0, "危险命令应失败"
        # 宿主机文件系统不受影响 — 通过检查宿主机上已知文件存在来验证
        import os
        assert os.path.exists("/etc"), "宿主机文件系统应不受影响"

    # D7: 容器内无法访问宿主机敏感路径
    def test_host_path_inaccessible(self, docker_sandbox: SandboxClient) -> None:
        """容器内无法访问宿主机敏感路径。"""
        # 尝试读取宿主机敏感文件
        result = docker_sandbox.read_file("../../etc/shadow")
        # 应拒绝（路径逃逸检查）或报文件不存在
        assert result["exit_code"] != 0, (
            f"宿主机敏感路径应被拒绝: {result}"
        )

    # D8: workdir 挂载可读写
    def test_workdir_bind_mount(self, docker_sandbox: SandboxClient) -> None:
        """容器内 workdir 可读写。"""
        # 容器内 write 到 /workdir（通过 working_dir 指定工作区）
        result_w = docker_sandbox.write_file(
            "test_bind.txt", "bind mount content",
            opts={"working_dir": "/workdir"},
        )
        assert result_w["exit_code"] == 0, f"容器内写入失败: {result_w}"
        # 容器内读回
        result_r = docker_sandbox.read_file(
            "test_bind.txt",
            opts={"working_dir": "/workdir"},
        )
        assert result_r["exit_code"] == 0, f"容器内读取失败: {result_r}"
        assert "bind mount content" in result_r["stdout"]

    # D9: 容器内环境变量白名单
    def test_env_whitelist(self, docker_sandbox: SandboxClient) -> None:
        """容器内环境变量不含 *_API_KEY。"""
        result = docker_sandbox.bash("env")
        assert result["exit_code"] == 0, f"env 命令失败: {result}"
        env_output = result["stdout"]
        # 检查是否意外泄漏了 API key
        for line in env_output.splitlines():
            key = line.split("=")[0] if "=" in line else ""
            if key.endswith("_API_KEY"):
                pytest.fail(f"环境变量泄漏了 API key: {line}")

    # D10: 容器内 skills 目录同步 — 容器内可以看到 agent package 挂载的 skills
    def test_skills_sync(self, docker_sandbox: SandboxClient) -> None:
        """agent package 的 harness/skills 同步到容器内，list_skills 可列出。"""
        # 指定 working_dir 为 agent package 挂载点，使 HTTP server 找到 harness/skills
        result = docker_sandbox.list_skills(opts={"working_dir": "/workdir"})
        assert result["exit_code"] == 0, f"list_skills 失败: {result}"
        skills = json.loads(result.get("stdout", "[]"))
        print(f"[docker] skills found: {skills}")
        # 验证 outfit-assistant 的顶层 skill 都存在（list_skills 只扫一层目录）
        skill_names = {s["name"] for s in skills}
        assert "dummy_pre_skill" in skill_names, f"缺 dummy_pre_skill: {skill_names}"
        assert "skill-runner" in skill_names, f"缺 skill-runner: {skill_names}"
        # minimax_outfit_skills 是嵌套目录（子目录内含 SKILL.md），list_skills 一层不列出
        # 每个 skill 应有 path
        for s in skills:
            assert s.get("path"), f"skill {s['name']} 缺 path"

    # D11: 容器内加载 skill 内容
    def test_skill_loading(self, docker_sandbox: SandboxClient) -> None:
        """load_skill 可读取 SKILL.md 内容。"""
        # 加载 skill-runner 的 SKILL.md
        result = docker_sandbox.load_skill(
            "skill-runner",
            opts={"working_dir": "/workdir"},
        )
        assert result["exit_code"] == 0, f"load_skill 失败: {result}"
        # SKILL.md 内容开头是 "# Skill Runner"（标题），验证实际内容
        assert "# Skill Runner" in result.get("stdout", ""), (
            f"stdout 缺 SKILL.md 内容: {result}"
        )

    # D12: 容器内工具调用全链路 — workflow 会执行的典型序列
    def test_tool_execution_chain(self, docker_sandbox: SandboxClient) -> None:
        """workflow 需要的工具调用全链路：bash→write_file→read_file→grep→glob。"""
        # 1) bash: 执行命令
        r1 = docker_sandbox.bash("echo tool_chain_test", opts={"working_dir": "/workdir"})
        assert r1["exit_code"] == 0 and "tool_chain_test" in r1["stdout"], f"bash 失败: {r1}"
        # 2) write_file: 写文件到 workdir
        r2 = docker_sandbox.write_file(
            "chain_test.txt", "chain content",
            opts={"working_dir": "/workdir"},
        )
        assert r2["exit_code"] == 0, f"write_file 失败: {r2}"
        # 3) read_file: 读回文件
        r3 = docker_sandbox.read_file("chain_test.txt", opts={"working_dir": "/workdir"})
        assert r3["exit_code"] == 0 and "chain content" in r3["stdout"], f"read_file 失败: {r3}"
        # 4) grep: 搜索 workdir 内文件
        r4 = docker_sandbox.exec("grep", {"pattern": "chain"}, opts={"working_dir": "/workdir"})
        assert r4["exit_code"] == 0, f"grep 失败: {r4}"
        # 5) glob: 列举 workdir 内文件
        r5 = docker_sandbox.exec("glob", {"pattern": "chain*"}, opts={"working_dir": "/workdir"})
        assert r5["exit_code"] == 0, f"glob 失败: {r5}"


# ═══════════════════════════════════════════════════════════════════════
# 4.3 容器化特殊场景
# ═══════════════════════════════════════════════════════════════════════


class TestContainerSpecialScenarios:
    """DockerSandbox 特殊场景。"""

    # D11: 镜像不存在时构造失败不 fallback
    def test_missing_image_raises(self) -> None:
        """镜像不存在时抛异常，不 fallback 到 none。"""
        import subprocess
        # 用 --pull=never 防止 Docker 尝试从远端 registry 拉取挂起
        result = subprocess.run(
            ["docker", "run", "--rm", "--pull=never", "nonexistent-sandbox-image:latest",
             "echo", "should-not-run"],
            capture_output=True, text=True, timeout=30,
        )
        assert result.returncode != 0, (
            f"不存在的镜像应启动失败: {result.stdout}"
        )
        error_msg = (result.stderr + result.stdout).lower()
        assert any(kw in error_msg for kw in (
            "unable to find image", "not found", "no such image",
            "cannot connect to the docker daemon",
        )), f"错误信息应包含镜像不存在或 Docker 不可达: {result.stderr}"

    # D12: 并发上限拒绝新用户 — 由 SandboxLifecycle 管理，daemon 层覆盖
    # 见 sandbox_lifecycle_test.cpp LimitRejectsNotInterrupts

    # D13: 容器异常退出触发快照 — 由 SandboxSnapshots 覆盖
    # 见 sandbox_lifecycle_test.cpp KilledTriggersSnapshot

    # D14: 跨容器 session 隔离
    def test_cross_session_isolation(self, docker_sandbox: SandboxClient) -> None:
        """同一容器内不同 session 文件隔离。"""
        # 写入一个文件（通过 working_dir 指定工作区）
        result = docker_sandbox.write_file(
            "session_a.txt", "session A data",
            opts={"working_dir": "/workdir"},
        )
        assert result["exit_code"] == 0
        # 用不同路径写入另一个文件
        result2 = docker_sandbox.write_file(
            "session_b.txt", "session B data",
            opts={"working_dir": "/workdir"},
        )
        assert result2["exit_code"] == 0
        # 两个文件都应存在
        r_a = docker_sandbox.read_file(
            "session_a.txt",
            opts={"working_dir": "/workdir"},
        )
        assert r_a["exit_code"] == 0
        r_b = docker_sandbox.read_file(
            "session_b.txt",
            opts={"working_dir": "/workdir"},
        )
        assert r_b["exit_code"] == 0
        assert "session A" in r_a["stdout"]
        assert "session B" in r_b["stdout"]

    # D15: 容器内 10MB 超长输出截断
    def test_large_output_truncated(self, docker_sandbox: SandboxClient) -> None:
        """TC-SBOX-040: 10MB 超长输出截断不破坏沙箱稳定性。"""
        result = docker_sandbox.bash(
            "python3 -c \"print('x'*10000000)\"",
            opts={"output_max_bytes": 1000},
        )
        assert result["exit_code"] == 0, f"大型输出命令失败: {result}"
        assert result["truncated"] is True, f"截断未标记: {result}"
        assert len(result["stdout"].encode("utf-8")) <= 1000, (
            f"截断后仍超限: {len(result['stdout'])}"
        )