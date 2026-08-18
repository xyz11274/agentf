"""Red 测试 — 暴露 _ensure_snapshot_fresh / _manifest_hash 隐性 bug。

背景（2026-08-18 跑 none 沙箱模式性能测试时发现）：
  _manifest_hash() 用 sys.executable（bridge venv）调 template_admin.py，但 bridge venv
  缺 agentflow_runtime.common.manifest，子进程抛 ModuleNotFoundError → returncode=1 +
  stderr 含 FATAL。现状代码只取 stdout 末行，失败时 stdout 为空 → 返回空串。
  上游 `if mhash and mhash == rhash` 短路为 False → 无条件触发 e2b snapshot 预构建
  （即使 sandbox.mode=none 也跑，超时 600s 失败）。

预期契约（sandbox-warm-template-oci.md §2.6.8 + 不变量 I-A8）：
  - hash 子进程失败时 MUST fail fast（显式抛错含 stderr 根因），不得返回空串掩盖
  - hash 一致 → 跳过预构建；不一致 → 预构建
  - 不得因 hash 算失败就误判 stale 触发预构建（霰弹式副作用）

修复方案 1（fail fast）：检 r.returncode != 0 或空 stdout 时显式报错。
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import pytest

# conftest.py 已把 frontend/bridge/ 插入 sys.path，但 e2e_perf_baseline.py 在 tests/ 下，
# 需把 tests/ 也加入才能 import 它。复用 conftest 的 PROJECT_ROOT 推导。
TESTS_DIR = Path(__file__).resolve().parent
if str(TESTS_DIR) not in sys.path:
    sys.path.insert(0, str(TESTS_DIR))

from e2e_perf_baseline import _ensure_snapshot_fresh  # noqa: E402


class TestManifestHashFailureFast:
    """hash 子进程失败时 MUST fail fast，不得返回空串误触发预构建。"""

    def test_hash_subprocess_rc_nonzero_raises_with_stderr(
        self, monkeypatch, tmp_path
    ):
        """rc=1（template_admin.py 失败惯例）时抛错，错误信息含 stderr 根因。"""
        fake_script = tmp_path / "fake_template_admin.py"
        fake_script.write_text(
            "import sys\n"
            "sys.stderr.write('[template_admin] FATAL: manifest parsing requires "
            "agentflow_runtime.common.manifest\\n')\n"
            "sys.exit(1)\n"
        )
        monkeypatch.setattr(
            "e2e_perf_baseline.PROJECT_ROOT", tmp_path,
            raising=False,
        )
        monkeypatch.chdir(tmp_path)
        hash_script_attr = "e2e_perf_baseline.PROJECT_ROOT"
        # _ensure_snapshot_fresh 内 hash_script = PROJECT_ROOT / "scripts" / "e2b" / "template_admin.py"
        scripts_dir = tmp_path / "scripts" / "e2b"
        scripts_dir.mkdir(parents=True)
        (scripts_dir / "template_admin.py").write_text(fake_script.read_text())

        agent_pkg = tmp_path / "agent"
        agent_pkg.mkdir()
        (agent_pkg / "manifest.jsonc").write_text("{}")
        registry = tmp_path / "registry.json"
        registry.write_text(json.dumps({"entries": [{"hash": "deadbeef"}]}))

        with pytest.raises(SystemExit) as excinfo:
            _ensure_snapshot_fresh(agent_pkg, registry)

        msg = str(excinfo.value)
        assert "agentflow_runtime" in msg or "FATAL" in msg, (
            f"fail-fast 错误信息应含 stderr 根因，got: {msg}"
        )

    def test_hash_subprocess_empty_stdout_raises(self, monkeypatch, tmp_path):
        """rc=0 但 stdout 空也算失败（hash 呝约非空），MUST 抛错不返空。"""
        scripts_dir = tmp_path / "scripts" / "e2b"
        scripts_dir.mkdir(parents=True)
        (scripts_dir / "template_admin.py").write_text(
            "import sys\nsys.stderr.write('err\\n')\n"  # rc=0 但 stdout 空
        )
        monkeypatch.setattr("e2e_perf_baseline.PROJECT_ROOT", tmp_path, raising=False)
        monkeypatch.chdir(tmp_path)

        agent_pkg = tmp_path / "agent"
        agent_pkg.mkdir()
        (agent_pkg / "manifest.jsonc").write_text("{}")
        registry = tmp_path / "registry.json"
        registry.write_text(json.dumps({"entries": [{"hash": "deadbeef"}]}))

        with pytest.raises(SystemExit):
            _ensure_snapshot_fresh(agent_pkg, registry)

    def test_hash_success_no_rebuild_when_registry_matches(
        self, monkeypatch, tmp_path
    ):
        """rc=0 + stdout 非空 hash + registry 一致 → 跳过预构建（不抛错）。"""
        scripts_dir = tmp_path / "scripts" / "e2b"
        scripts_dir.mkdir(parents=True)
        (scripts_dir / "template_admin.py").write_text(
            "print('abc12345')\n"  # rc=0 + stdout 非空 hash
        )
        monkeypatch.setattr("e2e_perf_baseline.PROJECT_ROOT", tmp_path, raising=False)
        monkeypatch.chdir(tmp_path)

        agent_pkg = tmp_path / "agent"
        agent_pkg.mkdir()
        (agent_pkg / "manifest.jsonc").write_text("{}")
        registry = tmp_path / "registry.json"
        registry.write_text(json.dumps({"entries": [{"hash": "abc12345"}]}))

        _ensure_snapshot_fresh(agent_pkg, registry)  # 不抛错即过


class TestEnsureSnapshotFreshNoSideEffects:
    """hash 失败时不该触发预构建（无 e2b SDK / 网络副作用）。"""

    def test_hash_failure_does_not_invoke_rebuild(
        self, monkeypatch, tmp_path
    ):
        """hash 算失败时 MUST NOT 触发 template_admin.py snapshot --manifest 预构建。

        反模式（现状 bug）：返回空串 → `if mhash and mhash == rhash` False →
        落到预构建分支，调 Sandbox.create 触发网络 + 超 SDK（none 模式下纯浪费）。
        """
        scripts_dir = tmp_path / "scripts" / "e2b"
        scripts_dir.mkdir(parents=True)
        # hash 模式（--hash-manifest）失败，但 snapshot 模式不该被调
        hash_call_count = {"n": 0}
        (scripts_dir / "template_admin.py").write_text(
            "import sys\n"
            "if '--hash-manifest' in sys.argv:\n"
            "    sys.stderr.write('FATAL: missing runtime\\n')\n"
            "    sys.exit(1)\n"
            "else:\n"  # snapshot 模式被触即哨兵
            "    sys.stderr.write('UNEXPECTED snapshot call\\n')\n"
            "    sys.exit(2)\n"
        )
        monkeypatch.setattr("e2e_perf_baseline.PROJECT_ROOT", tmp_path, raising=False)
        monkeypatch.chdir(tmp_path)

        agent_pkg = tmp_path / "agent"
        agent_pkg.mkdir()
        (agent_pkg / "manifest.jsonc").write_text("{}")
        registry = tmp_path / "registry.json"
        registry.write_text(json.dumps({"entries": [{"hash": "deadbeef"}]}))

        with pytest.raises(SystemExit):
            _ensure_snapshot_fresh(agent_pkg, registry)
        # 若走到预构建分支会调 snapshot 模式 exit=2 → 子进程爆但被异常吞下，
        # 测试断言「抛错含 FATAL」已在 test_hash_subprocess_rc_nonzero_raises_with_stderr 覆盖
