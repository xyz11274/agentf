"""TDD — artifact 路由键应使用 {user_id}_{client_id} 而非执行槽位 AF_SESSION_ID。

背景（docs/design/features/sandbox/artifact-storage.md §2.2）：
  artifact_id = {agent_name}/{session_id}/{file_path}
  其中 session_id := agentflow 的 {user_id}_{client_id}

但代码现状（skill_executor.py L724、L921）读 AF_SESSION_ID（执行槽位 0/1/2）
冒充 session_id 段，导致产物目录变成 outputs/0/、artifact_id 变成 agent/0/x.png，
丢失 user/client 维度，不同用户在同一槽位的产物会撞路径。

本套测试锁住"产物路由键使用 {user_id}_{client_id}"这一不变量（I-5 单一真相来源）。
"""

from __future__ import annotations

from pathlib import Path

import pytest


# ═══════════════════════════════════════════════════════════════════
# runtime_context.artifact_session_id — 路由键构造
# ═══════════════════════════════════════════════════════════════════


def test_artifact_session_id_combines_user_and_client(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """AF_USER_ID + AF_CLIENT_ID 同时就位 → 返回 '{uid}_{cid}'。"""
    monkeypatch.setenv("AF_USER_ID", "user_abc")
    monkeypatch.setenv("AF_CLIENT_ID", "client_123")
    from agentflow_runtime.common.runtime_context import artifact_session_id
    assert artifact_session_id() == "user_abc_client_123", (
        "artifact 路由键应为 '{user_id}_{client_id}'（artifact-storage.md §2.2）"
    )


def test_artifact_session_id_empty_when_both_unset(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """AF_USER_ID 与 AF_CLIENT_ID 均未注入 → 返回空串（哨兵：路由键未就位）。"""
    monkeypatch.delenv("AF_USER_ID", raising=False)
    monkeypatch.delenv("AF_CLIENT_ID", raising=False)
    from agentflow_runtime.common.runtime_context import artifact_session_id
    assert artifact_session_id() == "", (
        "user_id 与 client_id 均未就位时路由键应为空串哨兵，不得退化为槽位号"
    )


def test_artifact_session_id_does_not_use_session_slot(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """AF_SESSION_ID（执行槽位 0/1/2）不得冒充 artifact 路由键。"""
    monkeypatch.setenv("AF_SESSION_ID", "0")
    monkeypatch.setenv("AF_USER_ID", "user_abc")
    monkeypatch.setenv("AF_CLIENT_ID", "client_123")
    from agentflow_runtime.common.runtime_context import artifact_session_id
    assert artifact_session_id() == "user_abc_client_123", (
        "路由键必须取自 user_id/client_id，AF_SESSION_ID（槽位）不得混入"
    )


# ═══════════════════════════════════════════════════════════════════
# _ensure_artifacts_in_package — 产物落地目录段
# ═══════════════════════════════════════════════════════════════════


def _make_artifact(tmp_path: Path, rel: str = "assets/look.png") -> Path:
    """在 tmp_path 下造一个真实产物文件，返回绝对路径。"""
    p = tmp_path / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(b"fake-image")
    return p


def test_ensure_artifacts_uses_user_client_id_for_outputs_dir(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """产物应落到 outputs/{user_id}_{client_id}/ 而非 outputs/{slot}/。

    不变量（artifact-storage.md §2.2 + I-5 单一真相来源）：
      artifact 路由键必须是 {user_id}_{client_id}，AF_SESSION_ID 槽位号
      缺用户维度会导致跨用户产物撞路径。
    """
    monkeypatch.setenv("AF_SESSION_ID", "0")  # 执行槽位，不得进产物路径
    monkeypatch.setenv("AF_USER_ID", "user_abc")
    monkeypatch.setenv("AF_CLIENT_ID", "client_123")
    monkeypatch.setattr(
        "agentflow_runtime.skill_executor.AGENT_PACKAGE_DIR", str(tmp_path)
    )
    # 禁用沙箱回拉与上传副作用，只测产物目录段
    monkeypatch.setenv("AF_SANDBOX_ENDPOINT", "")
    monkeypatch.setenv("AF_ARTIFACT_STORAGE_URL", "")

    from agentflow_runtime.skill_executor import _ensure_artifacts_in_package
    # 产物在包外（模拟 LLM 在 /tmp 或沙箱里写产物），触发复制到 outputs/{sess}/
    # 注意：tmp_path 被设为 AGENT_PACKAGE_DIR，产物必须放在 tmp_path 之外的独立目录，
    # 否则会被 _ensure_artifacts_in_package 当作"包内"走 kept_in_place 分支不复制。
    outside_dir = tmp_path.parent / "outside_artifacts"
    outside_dir.mkdir(exist_ok=True)
    artifact = outside_dir / "outside-look.png"
    artifact.write_bytes(b"fake-image")
    result = _ensure_artifacts_in_package([str(artifact)])

    expected_dir = tmp_path / "outputs" / "user_abc_client_123"
    expected_path = expected_dir / "outside-look.png"
    assert expected_path.is_file(), (
        f"产物应落到 {expected_path}（{expected_dir}/），"
        f"实际 result={result}，outputs/ 下内容={list((tmp_path / 'outputs').rglob('*'))}"
    )
    # 槽位号 0 不得出现在产物路径里
    assert not (tmp_path / "outputs" / "0").exists(), (
        "产物目录不得使用执行槽位号 0（缺用户维度）"
    )


def test_ensure_artifacts_outputs_dir_empty_when_user_client_unset(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """user_id/client_id 均未注入时退化为 outputs/（legacy 路径），不得用槽位号。"""
    monkeypatch.setenv("AF_SESSION_ID", "1")  # 槽位，不得进产物路径
    monkeypatch.delenv("AF_USER_ID", raising=False)
    monkeypatch.delenv("AF_CLIENT_ID", raising=False)
    monkeypatch.setattr(
        "agentflow_runtime.skill_executor.AGENT_PACKAGE_DIR", str(tmp_path)
    )
    monkeypatch.setenv("AF_SANDBOX_ENDPOINT", "")
    monkeypatch.setenv("AF_ARTIFACT_STORAGE_URL", "")

    from agentflow_runtime.skill_executor import _ensure_artifacts_in_package
    outside_dir = tmp_path.parent / "outside_artifacts_2"
    outside_dir.mkdir(exist_ok=True)
    artifact = outside_dir / "outside-look.png"
    artifact.write_bytes(b"fake-image")
    result = _ensure_artifacts_in_package([str(artifact)])

    expected_path = tmp_path / "outputs" / "outside-look.png"
    assert expected_path.is_file(), (
        f"user_id/client_id 未就位时应退化为 outputs/（legacy），"
        f"实际 result={result}，outputs/ 下内容={list((tmp_path / 'outputs').rglob('*'))}"
    )
    assert not (tmp_path / "outputs" / "1").exists(), (
        "槽位号 1 不得进产物路径，即使 user/client 缺失也只用 legacy outputs/"
    )


# ═══════════════════════════════════════════════════════════════════
# _upload_artifacts_to_storage — 上传 body 的 session_id 段
# ═══════════════════════════════════════════════════════════════════


def test_upload_artifacts_uses_user_client_id_for_session_id(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """上传 body 的 session_id 段必须是 '{user_id}_{client_id}'，不得用槽位号。

    不变量（artifact-storage.md §I-artifact-id）：
      artifact_id = {agent_name}/{session_id}/{file_path}
      session_id 必须是 {user_id}_{client_id}（含用户维度）。
    """
    monkeypatch.setenv("AF_SESSION_ID", "0")  # 槽位，不得进上传 body
    monkeypatch.setenv("AF_USER_ID", "user_abc")
    monkeypatch.setenv("AF_CLIENT_ID", "client_123")
    monkeypatch.setattr(
        "agentflow_runtime.skill_executor.AGENT_PACKAGE_DIR", str(tmp_path)
    )
    # 指向一个假 bridge endpoint，捕获上传 body
    captured: dict = {}
    captured_events: list[tuple[str, dict]] = []

    def _capture_event(event_name, payload, **_kwargs):
        captured_events.append((event_name, payload))
        return payload

    monkeypatch.setattr("agentflow_runtime.hooks.trigger", _capture_event)

    class _FakeResp:
        def __init__(self):
            self._body = b'{"artifact_ids": ["fake-id"]}'

        def read(self):
            return self._body

        def __enter__(self):
            return self

        def __exit__(self, *args):
            pass

    def _fake_urlopen(req, timeout=10):
        captured["url"] = req.full_url
        captured["body"] = req.data.decode("utf-8")
        captured["method"] = req.method
        return _FakeResp()

    monkeypatch.setenv("AF_ARTIFACT_STORAGE_URL", "http://fake-bridge:8199")
    monkeypatch.setattr("urllib.request.urlopen", _fake_urlopen)

    # 造一个真实可读的产物文件
    artifact = tmp_path / "look.png"
    artifact.write_bytes(b"fake-image")

    from agentflow_runtime.skill_executor import _upload_artifacts_to_storage
    _upload_artifacts_to_storage([str(artifact)])

    # 上传 body 必须含 user_abc_client_123，不得含裸槽位号 "0"
    assert "user_abc_client_123" in captured.get("body", ""), (
        f"上传 body 的 session_id 段应为 'user_abc_client_123'，"
        f"实际 body={captured.get('body')}"
    )
    # artifact_id 不得退化为 agent/0/look.png
    assert '"session_id": "0"' not in captured.get("body", ""), (
        f"上传 body 不得用槽位号 0 作 session_id，实际 body={captured.get('body')}"
    )

    assert len(captured_events) == 1
    event_name, payload = captured_events[0]
    assert event_name == "artifact.storage_result"
    assert payload["status"] == "uploaded"
    assert payload["attempted_count"] == 1
    assert payload["uploaded_count"] == 1
    entry = payload["entries"][0]
    assert entry["status"] == "uploaded"
    assert entry["artifact_id"] == "fake-id"
    assert entry["size_bytes"] == len(b"fake-image")
    assert len(entry["sha256"]) == 64
