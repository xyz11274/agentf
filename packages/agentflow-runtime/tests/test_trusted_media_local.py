"""Local media and file tools share one user/conversation workspace."""
import pytest

from agentflow_runtime.common.runtime_context import artifact_session_id
from agentflow_runtime.executors.http import HTTPExecutor
from agentflow_runtime.security import trusted_media as media
from agentflow_runtime.security.secrets import _reset_registry_for_tests


@pytest.fixture
def workspace(monkeypatch, tmp_path):
    _reset_registry_for_tests()
    monkeypatch.setenv("AF_SANDBOX_MODE", "local")
    monkeypatch.setenv("AF_AGENT_PACKAGE_DIR", str(tmp_path))
    monkeypatch.setenv("AF_USER_ID", "user-a")
    monkeypatch.setenv("AF_CLIENT_ID", "conversation-a")
    yield tmp_path / "workspace" / artifact_session_id()
    _reset_registry_for_tests()


@pytest.mark.parametrize("endpoint", ["", "http://sandbox.invalid"])
def test_publish_then_read_uses_file_tool_workspace(workspace, tmp_path, monkeypatch, endpoint):
    def unexpected_transfer(*args, **kwargs):
        pytest.fail("Local media must not fall back to the shared artifact endpoint")
    monkeypatch.setattr(media, "_post_json", unexpected_transfer)
    source = tmp_path / "generated.png"
    source.write_bytes(b"safe generated image")
    published = media.publish_media(str(source), "assets/card.png", endpoint)
    expected = workspace / "assets/card.png"
    assert published == str(expected)
    assert expected.read_bytes() == source.read_bytes()
    assert not (tmp_path / "assets/card.png").exists()
    executor = HTTPExecutor(endpoint)
    for path in (published, "assets/card.png"):
        assert executor._local_path_scope("read_file", path) == (str(expected), str(workspace))
        with media.materialize_media(path, endpoint) as local:
            assert local == str(expected)
    qualified = expected.relative_to(tmp_path).as_posix()
    assert media.publish_media(published, qualified, endpoint) == published


def test_missing_local_media_does_not_read_shared_package_or_sandbox(workspace, tmp_path, monkeypatch):
    shared = tmp_path / "assets/card.png"
    shared.parent.mkdir()
    shared.write_bytes(b"another conversation")
    def unexpected_transfer(*args, **kwargs):
        pytest.fail("missing private media must not use shared package assets")
    monkeypatch.setattr(media, "_post_json", unexpected_transfer)
    with pytest.raises(media.TrustedMediaError, match="not found"):
        with media.materialize_media("assets/card.png", "http://sandbox.invalid"):
            pass


@pytest.mark.parametrize("kind", ["other_user", "symlink", "root_symlink", "missing_identity"])
def test_local_media_rejects_cross_workspace_access(workspace, tmp_path, monkeypatch, kind):
    source = tmp_path / "generated.png"
    source.write_bytes(b"safe generated image")
    other = tmp_path / "workspace/other-user"
    other.mkdir(parents=True)
    (other / "card.png").write_bytes(b"private image")
    target = "assets/card.png"
    if kind == "other_user":
        target = "workspace/other-user/card.png"
    elif kind == "symlink":
        workspace.mkdir()
        (workspace / "assets").symlink_to(other, target_is_directory=True)
    elif kind == "root_symlink":
        workspace.symlink_to(other, target_is_directory=True)
    else:
        monkeypatch.delenv("AF_CLIENT_ID")
    with pytest.raises(media.TrustedMediaError):
        media.publish_media(str(source), target, "http://sandbox.invalid")
    with pytest.raises(media.TrustedMediaError):
        with media.materialize_media(target, "http://sandbox.invalid"):
            pass
    assert (other / "card.png").read_bytes() == b"private image"


def test_remote_media_transfer_keeps_sandbox_relative_paths(workspace, tmp_path, monkeypatch):
    monkeypatch.setenv("AF_SANDBOX_MODE", "docker")
    requests = []
    monkeypatch.setattr(media, "_post_json", lambda *args, **kwargs: requests.append(args) or {})
    source = tmp_path / "generated.png"
    source.write_bytes(b"safe generated image")
    assert media.publish_media(str(source), "assets/card.png", "http://sandbox.invalid") == "assets/card.png"
    assert requests[0][:2] == ("http://sandbox.invalid", "/artifact/upload")
    assert requests[0][2]["path"] == "assets/card.png"
    assert not workspace.exists()
