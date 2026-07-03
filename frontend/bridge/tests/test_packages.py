"""Tests for AgentFlow Bridge — Agent Package HTTP routes (T5 + T8).

Covers:
* T5 regression — ``GET /api/packages`` still returns the on-disk list.
* T8 happy path — ``POST /api/packages/{name}/package`` runs the build
  script, computes a sha256, and returns the right envelope.
* T8 idempotent read — ``GET /api/packages/{name}/package`` returns
  metadata for an already-built tarball and 404 when none exists.
* T8 download — ``GET /api/packages/{name}/package/download`` streams
  the tarball back with the correct media type / filename.
* T8 error mapping — 404 for missing agent, 400 for missing package.json,
  500 + stderr envelope when the build script exits non-zero.
* Path-traversal guard — names containing ``..`` are rejected with 403.
"""

from __future__ import annotations

import asyncio
import gzip
import hashlib
import io
import json
import os
import subprocess
import tarfile
from pathlib import Path
from typing import Any
from unittest.mock import AsyncMock, patch

import pytest
from fastapi.testclient import TestClient

import config as config_module
from main import bridge_app as app


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


@pytest.fixture
def fake_packages_root(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Point ``settings.agent_packages_path`` at a temp dir and seed it.

    Three packages are created so we can assert list ordering and filtering:

    * ``basic_agent`` — has ``package.json`` so builds are allowed.
    * ``no_manifest`` — directory only; build should be rejected 400.
    * ``.hidden`` — leading dot; must be filtered out of the list.
    """
    root = tmp_path / "agent_packages"
    root.mkdir()

    (root / "basic_agent").mkdir()
    (root / "basic_agent" / "package.json").write_text(
        json.dumps({"name": "basic", "version": "1.0.0", "package_type": "release"})
    )

    # ``no_manifest`` is intentionally left empty — the build endpoint must
    # reject it with 400, and the list endpoint must still report it as a
    # zero-file package.
    (root / "no_manifest").mkdir()

    (root / ".hidden").mkdir()
    (root / ".hidden" / "package.json").write_text("{}")

    monkeypatch.setattr(config_module.settings, "agent_packages_dir", str(root))
    return root


@pytest.fixture
def fake_build_sh(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Replace ``publish/agent_build.sh`` with a tiny stub that we can drive.

    The stub accepts a ``MODE`` env var so a single fixture can simulate
    success, failure, or any other behaviour tests need. The default
    behaviour (MODE unset) produces two ``.tar.gz`` files in the
    ``output_dir`` argument — matching the real script's contract so the
    packaging endpoint can pick the most-recent one.
    """
    publish_dir = tmp_path / "publish"
    publish_dir.mkdir()
    build_sh = publish_dir / "agent_build.sh"
    build_sh.write_text(
        "#!/bin/bash\n"
        "set -e\n"
        "SRC=\"$2\"\n"
        "OUT=\"$3\"\n"
        "mkdir -p \"$OUT\"\n"
        'if [ "${BUILD_MODE:-ok}" = "fail" ]; then\n'
        '  echo "synthetic build failure" >&2\n'
        "  exit 17\n"
        "fi\n"
        'NAME=$(basename "$SRC")\n'
        'TARBALL="$OUT/${NAME}_1.0.0.tar.gz"\n'
        'tar -czf "$TARBALL" -C "$SRC" .\n'
        'OUTER="$OUT/${NAME}_release_1.0.0.tar.gz"\n'
        'cp "$TARBALL" "$OUTER"\n'
    )
    build_sh.chmod(0o755)

    import routes.packages as pkg_routes

    monkeypatch.setattr(pkg_routes, "_AGENT_BUILD_SH", build_sh)
    return build_sh


@pytest.fixture
def client(fake_packages_root: Path, fake_build_sh: Path) -> TestClient:
    return TestClient(app)


# ---------------------------------------------------------------------------
# T5 regression: list endpoint
# ---------------------------------------------------------------------------


def test_list_packages_returns_visible_dirs_and_filters_hidden(
    client: TestClient,
) -> None:
    """GET /api/packages returns a flat list of visible package dirs."""
    resp = client.get("/api/packages")
    assert resp.status_code == 200, resp.text
    body = resp.json()
    # T5 contract: response is a JSON array (List[PackageInfo]), NOT a
    # wrapped object. The flat-list shape is what the Vue frontend's
    # packageStore consumes directly.
    assert isinstance(body, list)
    names = [p["name"] for p in body]
    assert names == ["basic_agent", "no_manifest"]
    # Hidden dir is excluded.
    assert ".hidden" not in names
    # Each entry carries the documented keys; file_count may legitimately
    # be 0 (an empty dir is still a directory).
    for entry in body:
        assert set(entry.keys()) == {
            "name",
            "path",
            "file_count",
            "last_modified",
        }
        assert entry["file_count"] >= 0
    # ``basic_agent`` has a real package.json, so it must have >= 1 file.
    basic = next(p for p in body if p["name"] == "basic_agent")
    assert basic["file_count"] >= 1
    # ``no_manifest`` is empty.
    no_manifest = next(p for p in body if p["name"] == "no_manifest")
    assert no_manifest["file_count"] == 0


def test_list_packages_empty_root_returns_empty_list(
    client: TestClient,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """If the root dir has no subdirs, the response is an empty list (not 404)."""
    empty = tmp_path / "empty_agent_packages"
    empty.mkdir()
    monkeypatch.setattr(
        config_module.settings, "agent_packages_dir", str(empty)
    )
    resp = client.get("/api/packages")
    assert resp.status_code == 200
    assert resp.json() == []


# ---------------------------------------------------------------------------
# T8 happy path: POST /{name}/package
# ---------------------------------------------------------------------------


def test_post_package_runs_build_and_returns_envelope(
    client: TestClient,
    fake_packages_root: Path,
) -> None:
    """POST builds the tarball via the stub script and returns size + sha256."""
    resp = client.post("/api/packages/basic_agent/package")
    assert resp.status_code == 200, resp.text
    body = resp.json()

    # Every documented key is present.
    assert set(body.keys()) == {
        "path",
        "name",
        "size",
        "checksum",
        "last_modified",
    }
    # Tarball was actually created on disk under the package's dist dir.
    dist = fake_packages_root / "basic_agent" / "dist"
    assert dist.is_dir()
    tarballs = sorted(dist.glob("*.tar.gz"))
    assert len(tarballs) == 2  # inner + outer, matching agent_build.sh's contract
    # The envelope points at one of the produced tarballs.
    assert Path(body["path"]).parent == dist
    assert body["name"] in {p.name for p in tarballs}
    # Size and checksum agree with the on-disk file.
    chosen = dist / body["name"]
    assert body["size"] == chosen.stat().st_size
    expected = hashlib.sha256(chosen.read_bytes()).hexdigest()
    assert body["checksum"] == expected


def test_post_package_clears_stale_dist_before_rebuild(
    client: TestClient,
    fake_packages_root: Path,
) -> None:
    """A pre-existing dist/ should be wiped before the new build runs.

    Without this, callers could see metadata from a previous agent's run
    mixed in with the current one — a textbook F3 (I-3) violation where
    derived state drifts from content.
    """
    dist = fake_packages_root / "basic_agent" / "dist"
    dist.mkdir()
    (dist / "stale_artifact.tar.gz").write_bytes(b"stale")

    resp = client.post("/api/packages/basic_agent/package")
    assert resp.status_code == 200
    survivors = list(dist.glob("stale_artifact.tar.gz"))
    assert survivors == [], f"stale artefact was not removed: {survivors}"


# ---------------------------------------------------------------------------
# T8 error mapping
# ---------------------------------------------------------------------------


def test_post_package_missing_agent_returns_404(client: TestClient) -> None:
    resp = client.post("/api/packages/does_not_exist/package")
    assert resp.status_code == 404
    assert "does_not_exist" in resp.json()["detail"]


def test_post_package_missing_package_json_returns_400(
    client: TestClient,
) -> None:
    """A directory without package.json is a malformed package, not a 404."""
    resp = client.post("/api/packages/no_manifest/package")
    assert resp.status_code == 400
    assert "package.json" in resp.json()["detail"]


def test_post_package_path_traversal_returns_403(
    client: TestClient,
    fake_packages_root: Path,
) -> None:
    """Names with ``..`` must not escape the agent packages directory.

    Starlette rejects ``..`` as a URL path segment before the request
    reaches our handler, so we exercise the resolver directly to lock the
    defence in depth. The HTTP-layer test (``test_post_package_returns_404_for_nonexistent``)
    covers the end-to-end "request was rejected" case.
    """
    from routes.packages import _package_source_dir
    from fastapi import HTTPException

    with pytest.raises(HTTPException) as excinfo:
        _package_source_dir("..")
    assert excinfo.value.status_code == 403

    # Absolute-path injection must also be rejected.
    with pytest.raises(HTTPException) as excinfo:
        _package_source_dir("/etc/passwd")
    assert excinfo.value.status_code == 403


def test_post_package_build_failure_surfaces_stderr(
    client: TestClient,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Non-zero exit from the script becomes 500 with stderr in the body."""
    monkeypatch.setenv("BUILD_MODE", "fail")

    resp = client.post("/api/packages/basic_agent/package")
    assert resp.status_code == 500
    detail = resp.json()["detail"]
    # The structured envelope lets the UI render a useful error.
    assert isinstance(detail, dict)
    assert detail["exit_code"] == 17
    assert "synthetic build failure" in detail["stderr"]


def test_post_package_missing_build_script_returns_500(
    client: TestClient,
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """If ``agent_build.sh`` doesn't exist, surface a 500 with the path."""
    import routes.packages as pkg_routes

    missing = tmp_path / "no_such.sh"
    monkeypatch.setattr(pkg_routes, "_AGENT_BUILD_SH", missing)

    resp = client.post("/api/packages/basic_agent/package")
    assert resp.status_code == 500
    assert "agent_build.sh" in resp.json()["detail"]


# ---------------------------------------------------------------------------
# T8 read paths: GET /package, GET /package/download
# ---------------------------------------------------------------------------


def test_get_package_without_prior_build_returns_404(client: TestClient) -> None:
    resp = client.get("/api/packages/basic_agent/package")
    assert resp.status_code == 404
    assert "POST" in resp.json()["detail"]  # hint at the fix


def test_get_package_after_build_returns_metadata(
    client: TestClient,
) -> None:
    # Build first so dist/ has a tarball.
    build_resp = client.post("/api/packages/basic_agent/package")
    assert build_resp.status_code == 200
    built = build_resp.json()

    # Now GET must return the same envelope.
    resp = client.get("/api/packages/basic_agent/package")
    assert resp.status_code == 200
    body = resp.json()
    assert body["path"] == built["path"]
    assert body["name"] == built["name"]
    assert body["size"] == built["size"]
    assert body["checksum"] == built["checksum"]


def test_download_package_streams_tarball_with_correct_mime(
    client: TestClient,
) -> None:
    """GET .../download returns the tarball bytes with the documented headers."""
    client.post("/api/packages/basic_agent/package")  # populate dist/

    resp = client.get("/api/packages/basic_agent/package/download")
    assert resp.status_code == 200
    # FileResponse advertises application/gzip so the browser keeps the .gz.
    assert resp.headers["content-type"].startswith("application/gzip")
    # The Content-Disposition filename matches the on-disk tarball name.
    cd = resp.headers["content-disposition"]
    assert "filename=" in cd

    # Stream the response body and verify it's a valid gzip+tar archive.
    raw = b"".join(resp.iter_bytes())
    # Decompressing must succeed.
    decompressed = gzip.decompress(raw)
    # And it must look like a tar (member header magic at offset 257).
    assert len(decompressed) > 512
    assert decompressed[257:262] == b"ustar"


def test_download_package_without_build_returns_404(client: TestClient) -> None:
    resp = client.get("/api/packages/basic_agent/package/download")
    assert resp.status_code == 404


def test_download_package_path_traversal_returns_403(
    client: TestClient,
    fake_packages_root: Path,
) -> None:
    """Resolver-level 403 — see ``test_post_package_path_traversal_returns_403``."""
    from routes.packages import _package_source_dir
    from fastapi import HTTPException

    with pytest.raises(HTTPException) as excinfo:
        _package_source_dir("..")
    assert excinfo.value.status_code == 403


# ---------------------------------------------------------------------------
# Subprocess invocation contract — the only way the bridge actually talks
# to ``agent_build.sh`` is via ``asyncio.create_subprocess_exec``. These
# tests assert the exact argv we send so a future refactor can't silently
# drop or reorder arguments without breaking the build.
# ---------------------------------------------------------------------------


def test_post_package_invokes_subprocess_with_expected_argv(
    client: TestClient,
    fake_build_sh: Path,
) -> None:
    """POST must spawn ``bash <build.sh> build <src> <out>`` from project root.

    We patch ``asyncio.create_subprocess_exec`` to capture the call without
    actually running a subprocess (the on-disk artefact is produced by the
    fixture's stub script via the real path; here we just assert what
    argument vector the router constructed).
    """
    import routes.packages as pkg_routes

    real_create = asyncio.create_subprocess_exec

    captured: dict[str, Any] = {}

    async def spy(*args: Any, **kwargs: Any) -> Any:
        captured["args"] = args
        captured["kwargs"] = kwargs
        # Delegate to the real implementation so the side effects (tarball
        # on disk) still happen, and the response stays 200.
        return await real_create(*args, **kwargs)

    with patch.object(pkg_routes.asyncio, "create_subprocess_exec", side_effect=spy):
        resp = client.post("/api/packages/basic_agent/package")

    assert resp.status_code == 200, resp.text
    argv = captured["args"]
    # argv[0] = bash, argv[1] = build.sh, argv[2] = build, argv[3] = src, argv[4] = out
    assert argv[0] == "bash"
    assert Path(argv[1]) == fake_build_sh
    assert argv[2] == "build"
    # Source dir is the package's on-disk tree.
    assert Path(argv[3]).name == "basic_agent"
    # Output dir is the per-package dist/ directory.
    assert Path(argv[4]).name == "dist"
    # CWD is pinned so the script's own SCRIPT_DIR/PROJECT_ROOT agree.
    assert captured["kwargs"].get("cwd", "").endswith("aisf_agentflow")


# ---------------------------------------------------------------------------
# T5 — filesystem CRUD layer
# ---------------------------------------------------------------------------


@pytest.fixture
def crud_packages_root(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> Path:
    """Seed a minimal tree with nested files for the CRUD tests.

    Layout::

        basic_agent/
            agent.json     -> {"name": "basic"}
            nested/
                deep.txt   -> "hello"
    """
    root = tmp_path / "crud_agent_packages"
    root.mkdir()
    pkg = root / "basic_agent"
    pkg.mkdir()
    (pkg / "agent.json").write_text('{"name": "basic"}', encoding="utf-8")
    nested = pkg / "nested"
    nested.mkdir()
    (nested / "deep.txt").write_text("hello", encoding="utf-8")

    monkeypatch.setattr(
        config_module.settings, "agent_packages_dir", str(root)
    )
    return root


@pytest.fixture
def crud_client(crud_packages_root: Path) -> TestClient:
    """A TestClient that uses the CRUD-only fixture (no fake_build_sh)."""
    return TestClient(app)


# --- list -----------------------------------------------------------------


def test_t5_list_packages_returns_seeded(crud_client: TestClient) -> None:
    """GET /api/packages must include basic_agent with file_count = 2."""
    resp = crud_client.get("/api/packages")
    assert resp.status_code == 200
    body = resp.json()
    assert isinstance(body, list)
    names = [p["name"] for p in body]
    assert "basic_agent" in names
    entry = next(p for p in body if p["name"] == "basic_agent")
    assert entry["file_count"] == 2
    assert "last_modified" in entry
    assert "path" in entry


def test_t5_list_packages_excludes_hidden_dirs(
    crud_packages_root: Path, crud_client: TestClient
) -> None:
    (crud_packages_root / ".git").mkdir()
    (crud_packages_root / ".git" / "config").write_text("git")
    resp = crud_client.get("/api/packages")
    names = [p["name"] for p in resp.json()]
    assert ".git" not in names
    assert "basic_agent" in names


# --- tree -----------------------------------------------------------------


def test_t5_tree_returns_nested_structure(crud_client: TestClient) -> None:
    resp = crud_client.get("/api/packages/basic_agent/tree")
    assert resp.status_code == 200
    body = resp.json()

    def find(nodes, name):
        for n in nodes:
            if n["name"] == name:
                return n
        return None

    agent_json = find(body, "agent.json")
    assert agent_json is not None
    assert agent_json["type"] == "file"
    assert agent_json["path"] == "agent.json"

    nested = find(body, "nested")
    assert nested is not None
    assert nested["type"] == "dir"
    deep = find(nested["children"], "deep.txt")
    assert deep is not None
    assert deep["type"] == "file"
    assert deep["path"] == "nested/deep.txt"


def test_t5_tree_missing_package_returns_404(crud_client: TestClient) -> None:
    resp = crud_client.get("/api/packages/does_not_exist/tree")
    assert resp.status_code == 404


def test_t5_tree_dirs_sorted_before_files(
    crud_packages_root: Path, crud_client: TestClient
) -> None:
    pkg = crud_packages_root / "basic_agent"
    (pkg / "z_dir").mkdir()
    (pkg / "z_dir" / "x.txt").write_text("x")
    (pkg / "a_file.txt").write_text("a")
    body = crud_client.get("/api/packages/basic_agent/tree").json()
    dirs = [n for n in body if n["type"] == "dir"]
    files = [n for n in body if n["type"] == "file"]
    assert all(body.index(d) < body.index(f) for d in dirs for f in files)


# --- read file -----------------------------------------------------------


def test_t5_read_top_level(crud_client: TestClient) -> None:
    resp = crud_client.get("/api/packages/basic_agent/file/agent.json")
    assert resp.status_code == 200
    assert resp.json()["content"] == '{"name": "basic"}'


def test_t5_read_nested(crud_client: TestClient) -> None:
    resp = crud_client.get(
        "/api/packages/basic_agent/file/nested/deep.txt"
    )
    assert resp.status_code == 200
    assert resp.json()["content"] == "hello"


def test_t5_read_path_traversal_returns_403(crud_client: TestClient) -> None:
    """I-2: ``..`` segments must be rejected with 403 BEFORE FS access.

    Uses URL-encoded ``%2E%2E`` so Starlette's URL normalizer doesn't
    collapse the ``..`` segments into the path before the route is matched.
    """
    resp = crud_client.get(
        "/api/packages/basic_agent/file/%2E%2E/%2E%2E/etc/passwd"
    )
    assert resp.status_code == 403


def test_t5_read_absolute_path_returns_403(crud_client: TestClient) -> None:
    resp = crud_client.get("/api/packages/basic_agent/file//etc/passwd")
    assert resp.status_code == 403


def test_t5_read_missing_returns_404(crud_client: TestClient) -> None:
    resp = crud_client.get("/api/packages/basic_agent/file/missing.txt")
    assert resp.status_code == 404


def test_t5_read_directory_returns_409(crud_client: TestClient) -> None:
    resp = crud_client.get("/api/packages/basic_agent/file/nested")
    assert resp.status_code == 409


# --- write file (PUT) ----------------------------------------------------


def test_t5_put_then_get_roundtrip(crud_client: TestClient) -> None:
    target = "new_dir/nested.txt"
    payload = {"content": "round-trip body\nline 2"}
    put_resp = crud_client.put(
        f"/api/packages/basic_agent/file/{target}", json=payload
    )
    assert put_resp.status_code == 200, put_resp.text
    body = put_resp.json()
    assert body["path"] == target
    assert body["bytes"] == len(payload["content"].encode("utf-8"))
    get_resp = crud_client.get(f"/api/packages/basic_agent/file/{target}")
    assert get_resp.status_code == 200
    assert get_resp.json()["content"] == payload["content"]


def test_t5_put_overwrites_existing(crud_client: TestClient) -> None:
    crud_client.put(
        "/api/packages/basic_agent/file/agent.json",
        json={"content": "first"},
    )
    crud_client.put(
        "/api/packages/basic_agent/file/agent.json",
        json={"content": "second"},
    )
    assert (
        crud_client.get(
            "/api/packages/basic_agent/file/agent.json"
        ).json()["content"]
        == "second"
    )


def test_t5_put_path_traversal_returns_403(crud_client: TestClient) -> None:
    resp = crud_client.put(
        "/api/packages/basic_agent/file/%2E%2E/escape.txt",
        json={"content": "evil"},
    )
    assert resp.status_code == 403


# --- create empty file (POST) --------------------------------------------


def test_t5_post_create_empty_file(crud_client: TestClient) -> None:
    resp = crud_client.post("/api/packages/basic_agent/file/empty.txt")
    assert resp.status_code == 201
    assert resp.json()["path"] == "empty.txt"
    assert (
        crud_client.get(
            "/api/packages/basic_agent/file/empty.txt"
        ).json()["content"]
        == ""
    )


def test_t5_post_create_existing_returns_409(crud_client: TestClient) -> None:
    crud_client.post("/api/packages/basic_agent/file/agent.json")
    resp = crud_client.post("/api/packages/basic_agent/file/agent.json")
    assert resp.status_code == 409


def test_t5_post_create_path_traversal_returns_403(
    crud_client: TestClient,
) -> None:
    resp = crud_client.post(
        "/api/packages/basic_agent/file/%2E%2E/%2E%2E/etc/evil"
    )
    assert resp.status_code == 403


# --- delete file ---------------------------------------------------------


def test_t5_delete_file_then_get_404(crud_client: TestClient) -> None:
    crud_client.post("/api/packages/basic_agent/file/doomed.txt")
    assert (
        crud_client.delete(
            "/api/packages/basic_agent/file/doomed.txt"
        ).status_code
        == 204
    )
    assert (
        crud_client.get(
            "/api/packages/basic_agent/file/doomed.txt"
        ).status_code
        == 404
    )


def test_t5_delete_missing_returns_404(crud_client: TestClient) -> None:
    resp = crud_client.delete(
        "/api/packages/basic_agent/file/nonexistent.txt"
    )
    assert resp.status_code == 404


def test_t5_delete_directory_via_file_endpoint_returns_409(
    crud_client: TestClient,
) -> None:
    resp = crud_client.delete("/api/packages/basic_agent/file/nested")
    assert resp.status_code == 409


def test_t5_delete_path_traversal_returns_403(crud_client: TestClient) -> None:
    resp = crud_client.delete(
        "/api/packages/basic_agent/file/%2E%2E/escape"
    )
    assert resp.status_code == 403


# --- rename file ---------------------------------------------------------


def test_t5_rename_file_roundtrip(crud_client: TestClient) -> None:
    crud_client.put(
        "/api/packages/basic_agent/file/old.txt",
        json={"content": "data"},
    )
    resp = crud_client.post(
        "/api/packages/basic_agent/file/old.txt/rename",
        json={"new_name": "new.txt"},
    )
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["old_path"] == "old.txt"
    assert body["new_path"] == "new.txt"
    assert (
        crud_client.get(
            "/api/packages/basic_agent/file/old.txt"
        ).status_code
        == 404
    )
    assert (
        crud_client.get(
            "/api/packages/basic_agent/file/new.txt"
        ).json()["content"]
        == "data"
    )


def test_t5_rename_target_exists_returns_409(crud_client: TestClient) -> None:
    crud_client.put(
        "/api/packages/basic_agent/file/a.txt", json={"content": "a"}
    )
    crud_client.put(
        "/api/packages/basic_agent/file/b.txt", json={"content": "b"}
    )
    resp = crud_client.post(
        "/api/packages/basic_agent/file/a.txt/rename",
        json={"new_name": "b.txt"},
    )
    assert resp.status_code == 409


def test_t5_rename_rejects_path_separator_in_new_name(
    crud_client: TestClient,
) -> None:
    crud_client.put(
        "/api/packages/basic_agent/file/x.txt", json={"content": "x"}
    )
    resp = crud_client.post(
        "/api/packages/basic_agent/file/x.txt/rename",
        json={"new_name": "subdir/x.txt"},
    )
    assert resp.status_code == 400


def test_t5_rename_missing_returns_404(crud_client: TestClient) -> None:
    resp = crud_client.post(
        "/api/packages/basic_agent/file/ghost.txt/rename",
        json={"new_name": "new.txt"},
    )
    assert resp.status_code == 404


# --- directories ---------------------------------------------------------


def test_t5_create_directory(crud_client: TestClient) -> None:
    resp = crud_client.post("/api/packages/basic_agent/dir/subdir")
    assert resp.status_code == 201
    tree = crud_client.get("/api/packages/basic_agent/tree").json()
    assert any(n["name"] == "subdir" and n["type"] == "dir" for n in tree)


def test_t5_create_directory_creates_parents(crud_client: TestClient) -> None:
    resp = crud_client.post("/api/packages/basic_agent/dir/a/b/c")
    assert resp.status_code == 201
    tree = crud_client.get("/api/packages/basic_agent/tree").json()
    a = next(n for n in tree if n["name"] == "a")
    b = next(n for n in a["children"] if n["name"] == "b")
    c = next(n for n in b["children"] if n["name"] == "c")
    assert c["type"] == "dir"


def test_t5_create_existing_directory_returns_409(crud_client: TestClient) -> None:
    crud_client.post("/api/packages/basic_agent/dir/dup")
    resp = crud_client.post("/api/packages/basic_agent/dir/dup")
    assert resp.status_code == 409


def test_t5_create_directory_path_traversal_returns_403(
    crud_client: TestClient,
) -> None:
    resp = crud_client.post("/api/packages/basic_agent/dir/%2E%2E/escape")
    assert resp.status_code == 403


def test_t5_delete_empty_directory(crud_client: TestClient) -> None:
    crud_client.post("/api/packages/basic_agent/dir/empty_dir")
    resp = crud_client.delete("/api/packages/basic_agent/dir/empty_dir")
    assert resp.status_code == 204
    tree = crud_client.get("/api/packages/basic_agent/tree").json()
    assert not any(n["name"] == "empty_dir" for n in tree)


def test_t5_delete_non_empty_directory_returns_409(crud_client: TestClient) -> None:
    """Recursive delete is out of scope — non-empty dir must 409."""
    resp = crud_client.delete("/api/packages/basic_agent/dir/nested")
    assert resp.status_code == 409


def test_t5_delete_missing_directory_returns_404(crud_client: TestClient) -> None:
    resp = crud_client.delete("/api/packages/basic_agent/dir/never_existed")
    assert resp.status_code == 404


def test_t5_delete_file_via_dir_endpoint_returns_409(
    crud_client: TestClient,
) -> None:
    """DELETE /dir/{file} must NOT silently delete files."""
    resp = crud_client.delete("/api/packages/basic_agent/dir/agent.json")
    assert resp.status_code == 409


# --- invalid package names ----------------------------------------------


def test_t5_invalid_package_name_returns_403(crud_client: TestClient) -> None:
    resp = crud_client.get("/api/packages/.hidden/tree")
    assert resp.status_code == 403


# --- helper unit tests --------------------------------------------------


def test_t5_validate_relative_path_rejects_dotdot() -> None:
    from fastapi import HTTPException

    from routes.packages import _validate_relative_path

    for bad in ("../etc", "a/../b", "a/b/.."):
        with pytest.raises(HTTPException) as excinfo:
            _validate_relative_path(bad)
        assert excinfo.value.status_code == 403


def test_t5_validate_relative_path_rejects_absolute() -> None:
    from fastapi import HTTPException

    from routes.packages import _validate_relative_path

    for bad in ("/etc/passwd", "/x"):
        with pytest.raises(HTTPException) as excinfo:
            _validate_relative_path(bad)
        assert excinfo.value.status_code == 403


def test_t5_validate_relative_path_rejects_nul_and_backslash() -> None:
    from fastapi import HTTPException

    from routes.packages import _validate_relative_path

    for bad in ("a\x00b", "a\\b", "a\\..\\b"):
        with pytest.raises(HTTPException) as excinfo:
            _validate_relative_path(bad)
        assert excinfo.value.status_code == 403


def test_t5_resolve_within_rejects_symlink_escape(
    tmp_path: Path, crud_packages_root: Path
) -> None:
    """A symlink inside a package that escapes must be 403 (I-1, I-2)."""
    from fastapi import HTTPException

    from routes.packages import _resolve_within

    pkg_dir = crud_packages_root / "basic_agent"
    outside = tmp_path / "outside.txt"
    outside.write_text("secret")
    link = pkg_dir / "escape_link"
    os.symlink(str(outside), str(link))

    with pytest.raises(HTTPException) as excinfo:
        _resolve_within(pkg_dir, ["escape_link"])
    assert excinfo.value.status_code == 403


# --- update package (PUT) -------------------------------------------------


def _make_tar_gz(target_path: Path, files: dict[str, str]) -> Path:
    """Create a temporary ``.tar.gz`` archive with the given files.

    *files* maps relative paths to their text content.
    Returns the path to the created archive.
    """
    import gzip

    archive = target_path / "update.tar.gz"
    with tarfile.open(archive, "w:gz") as tf:
        for rel, content in files.items():
            info = tarfile.TarInfo(name=rel)
            encoded = content.encode("utf-8")
            info.size = len(encoded)
            tf.addfile(info, io.BytesIO(encoded))
    return archive


def test_update_incremental_adds_new_files(crud_client: TestClient, tmp_path: Path) -> None:
    """Incremental update must add new files without removing existing ones."""
    archive = _make_tar_gz(tmp_path, {"new_file.txt": "new content"})

    resp = crud_client.put(
        "/api/packages/basic_agent/update",
        files={"file": ("update.tar.gz", archive.read_bytes(), "application/gzip")},
        data={"mode": "incremental"},
    )
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["name"] == "basic_agent"

    # Existing file must still be there
    existing = crud_client.get("/api/packages/basic_agent/file/agent.json")
    assert existing.status_code == 200
    assert existing.json()["content"] == '{"name": "basic"}'

    # New file must have been added
    added = crud_client.get("/api/packages/basic_agent/file/new_file.txt")
    assert added.status_code == 200
    assert added.json()["content"] == "new content"


def test_update_incremental_overwrites_existing(crud_client: TestClient, tmp_path: Path) -> None:
    """Incremental update must overwrite files that exist in the archive."""
    archive = _make_tar_gz(tmp_path, {"agent.json": '{"name": "overwritten"}'})

    resp = crud_client.put(
        "/api/packages/basic_agent/update",
        files={"file": ("update.tar.gz", archive.read_bytes(), "application/gzip")},
        data={"mode": "incremental"},
    )
    assert resp.status_code == 200

    # Existing file must be overwritten
    overwritten = crud_client.get("/api/packages/basic_agent/file/agent.json")
    assert overwritten.status_code == 200
    assert overwritten.json()["content"] == '{"name": "overwritten"}'


def test_update_full_replaces_all_content(crud_client: TestClient, tmp_path: Path) -> None:
    """Full update must remove all existing files before extracting."""
    archive = _make_tar_gz(tmp_path, {"only.txt": "only file"})

    # Sanity: original files exist
    existing_before = crud_client.get("/api/packages/basic_agent/file/agent.json")
    assert existing_before.status_code == 200

    resp = crud_client.put(
        "/api/packages/basic_agent/update",
        files={"file": ("update.tar.gz", archive.read_bytes(), "application/gzip")},
        data={"mode": "full"},
    )
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["name"] == "basic_agent"

    # Old files must be gone
    old = crud_client.get("/api/packages/basic_agent/file/agent.json")
    assert old.status_code == 404
    old_nested = crud_client.get("/api/packages/basic_agent/file/nested/deep.txt")
    assert old_nested.status_code == 404

    # New file must be present
    new = crud_client.get("/api/packages/basic_agent/file/only.txt")
    assert new.status_code == 200
    assert new.json()["content"] == "only file"


def test_update_full_preserves_dist_directory(crud_client: TestClient, crud_packages_root: Path, tmp_path: Path) -> None:
    """Full update must preserve the ``dist/`` directory if it exists."""
    dist = crud_packages_root / "basic_agent" / "dist"
    dist.mkdir()
    (dist / "built.tar.gz").write_text("build artifact")

    archive = _make_tar_gz(tmp_path, {"new_root.txt": "fresh start"})

    resp = crud_client.put(
        "/api/packages/basic_agent/update",
        files={"file": ("update.tar.gz", archive.read_bytes(), "application/gzip")},
        data={"mode": "full"},
    )
    assert resp.status_code == 200

    # dist/ must be preserved
    assert (dist / "built.tar.gz").read_text() == "build artifact"

    # Old root files gone
    assert not (crud_packages_root / "basic_agent" / "agent.json").exists()


def test_update_missing_package_returns_404(crud_client: TestClient, tmp_path: Path) -> None:
    """Updating a non-existent package must return 404."""
    archive = _make_tar_gz(tmp_path, {"x.txt": "x"})
    resp = crud_client.put(
        "/api/packages/does_not_exist/update",
        files={"file": ("x.tar.gz", archive.read_bytes(), "application/gzip")},
    )
    assert resp.status_code == 404


def test_update_invalid_mode_returns_400(crud_client: TestClient, tmp_path: Path) -> None:
    """An invalid mode string must return 400."""
    archive = _make_tar_gz(tmp_path, {"x.txt": "x"})
    resp = crud_client.put(
        "/api/packages/basic_agent/update",
        files={"file": ("x.tar.gz", archive.read_bytes(), "application/gzip")},
        data={"mode": "invalid_mode"},
    )
    assert resp.status_code == 400


def test_update_unsupported_archive_returns_400(crud_client: TestClient, tmp_path: Path) -> None:
    """An unsupported file extension must return 400."""
    fake_archive = tmp_path / "bad.exe"
    fake_archive.write_bytes(b"not an archive")
    resp = crud_client.put(
        "/api/packages/basic_agent/update",
        files={"file": ("bad.exe", fake_archive.read_bytes(), "application/octet-stream")},
    )
    assert resp.status_code == 400


# --- router wiring ------------------------------------------------------


def test_t5_router_has_all_required_endpoints() -> None:
    """Sanity: the spec's 9 endpoints are wired on the router."""
    import routes.packages as pkg_route
    from fastapi.routing import APIRoute

    methods_paths = {
        (",".join(sorted(r.methods)), r.path)
        for r in pkg_route.router.routes
        if isinstance(r, APIRoute)
    }
    expected = {
        ("GET", "/api/packages"),
        ("GET", "/api/packages/{name}/tree"),
        ("GET", "/api/packages/{name}/file/{file_path:path}"),
        ("PUT", "/api/packages/{name}/file/{file_path:path}"),
        ("POST", "/api/packages/{name}/file/{file_path:path}"),
        ("DELETE", "/api/packages/{name}/file/{file_path:path}"),
        ("POST", "/api/packages/{name}/file/{file_path:path}/rename"),
        ("POST", "/api/packages/{name}/dir/{dir_path:path}"),
        ("DELETE", "/api/packages/{name}/dir/{dir_path:path}"),
        ("PUT", "/api/packages/{name}/update"),
    }
    assert expected.issubset(methods_paths), (
        f"missing endpoints: {expected - methods_paths}"
    )


# ---------------------------------------------------------------------------
# create-from-template e2e tests
# ---------------------------------------------------------------------------


@pytest.fixture
def fake_template_root(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Create a minimal harness_agent template stub under a temp dir.

    The stub is enough to verify merge behaviour without copying the real
    (hundreds-of-KB) template directory.
    """
    root = tmp_path / "agent_templates"
    tmpl = root / "harness_agent"
    tmpl.mkdir(parents=True)

    # agent.json — 4 infra tasks
    (tmpl / "agent.json").write_text(json.dumps({
        "name": "TemplateAgent",
        "version": "1.0.0",
        "description": "Template base",
        "session_pool_size": 1,
        "max_workflows": 5,
        "max_concurrent_workflows": 3,
        "data_bus_capacity_mb": 5,
        "tasks": [
            {"id": 0, "type": "dispatcher", "description": "infra dispatcher"},
            {"id": 1, "type": "coordinator", "description": "infra coordinator"},
            {"id": 2, "type": "skill", "description": "infra pre-skill"},
            {"id": 3, "type": "skill", "description": "infra skill-runner"},
        ],
    }))

    # agent.md — stub
    (tmpl / "agent.md").write_text("---\nname: TemplateAgent\n---\n\nTemplate stub")

    # package.json — stub
    (tmpl / "package.json").write_text('{"name": "template", "version": "0.0.0"}')

    # A workflow file with working_dir to test rewriting
    (tmpl / "coordinator_workflow.json").write_text(json.dumps({
        "id": "coordinator-workflow",
        "nodes": [{
            "id": "decide",
            "type": "task",
            "command": "./nodes/coordinator_decide.py",
            "working_dir": "examples/agent/outfit_assistant",
        }],
        "edges": [],
    }))

    # Template-owned infra directory
    (tmpl / "harness/tools").mkdir(parents=True)
    (tmpl / "harness/tools/bash.py").write_text("# template tool")

    # Template-owned nodes
    (tmpl / "nodes").mkdir()
    (tmpl / "nodes/databus_client.py").write_text("# template databus")
    (tmpl / "nodes/dummy_pre_skill.py").write_text("# template pre-skill")

    # Template-owned tests
    (tmpl / "tests").mkdir()
    (tmpl / "tests/test_template.py").write_text("# template test")

    # Point settings at our stub template root
    monkeypatch.setattr(config_module.settings, "agent_template_dir", str(root))

    return tmpl


@pytest.fixture
def template_client(
    fake_packages_root: Path,
    fake_template_root: Path,
) -> TestClient:
    return TestClient(app)


def _make_user_archive(
    tmp_path: Path,
    files: dict[str, str],
    fmt: str = "tar.gz",
) -> tuple[bytes, str]:
    """Create a temporary archive and return ``(content_bytes, filename)``."""
    if fmt == "tar.gz":
        archive = tmp_path / "user_agent.tar.gz"
        with tarfile.open(archive, "w:gz") as tf:
            for rel, content in files.items():
                info = tarfile.TarInfo(name=rel)
                encoded = content.encode("utf-8")
                info.size = len(encoded)
                tf.addfile(info, io.BytesIO(encoded))
    else:
        archive = tmp_path / "user_agent.zip"
        import zipfile
        with zipfile.ZipFile(archive, "w") as zf:
            for rel, content in files.items():
                zf.writestr(rel, content)

    return archive.read_bytes(), archive.name


def test_create_from_template_happy_path(
    template_client: TestClient,
    tmp_path: Path,
) -> None:
    """Happy path: minimal user archive + template = merged package."""
    content, filename = _make_user_archive(tmp_path, {
        "agent.md": "---\nname: MyAgent\n---\n\nMy custom agent",
    })
    resp = template_client.post(
        "/api/packages/create-from-template",
        files={"file": (filename, content, "application/gzip")},
        data={"name": "my-agent"},
    )
    assert resp.status_code == 201, resp.text
    body = resp.json()
    assert body["name"] == "my-agent"
    assert body["file_count"] > 0

    # Verify template infra files survived merge
    get = template_client.get("/api/packages/my-agent/file/harness/tools/bash.py")
    assert get.status_code == 200
    get = template_client.get("/api/packages/my-agent/file/nodes/databus_client.py")
    assert get.status_code == 200
    get = template_client.get("/api/packages/my-agent/file/tests/test_template.py")
    assert get.status_code == 200

    # Verify user's agent.md won
    get = template_client.get("/api/packages/my-agent/file/agent.md")
    assert get.status_code == 200
    assert get.json()["content"].startswith("---\nname: MyAgent")

    # Verify working_dir was rewritten
    get = template_client.get(
        "/api/packages/my-agent/file/coordinator_workflow.json"
    )
    assert get.status_code == 200
    wf = json.loads(get.json()["content"])
    assert wf["nodes"][0]["working_dir"] == "my-agent"


def test_create_from_template_agent_json_deep_merge(
    template_client: TestClient,
    tmp_path: Path,
) -> None:
    """User top-level fields override template; infra tasks preserved."""
    content, filename = _make_user_archive(tmp_path, {
        "agent.json": json.dumps({
            "name": "CustomAgent",
            "version": "2.0.0",
            "description": "Customized",
            "session_pool_size": 5,
        }),
    })
    resp = template_client.post(
        "/api/packages/create-from-template",
        files={"file": (filename, content, "application/gzip")},
        data={"name": "custom-merged"},
    )
    assert resp.status_code == 201, resp.text

    get = template_client.get("/api/packages/custom-merged/file/agent.json")
    assert get.status_code == 200
    cfg = json.loads(get.json()["content"])

    # User's top-level fields win
    assert cfg["name"] == "CustomAgent"
    assert cfg["version"] == "2.0.0"
    assert cfg["description"] == "Customized"
    assert cfg["session_pool_size"] == 5

    # Template infra tasks preserved (4 tasks)
    assert len(cfg["tasks"]) == 4
    assert cfg["tasks"][0]["id"] == 0
    assert cfg["tasks"][0]["type"] == "dispatcher"

    # User's custom fields that are not in the override allowlist are preserved
    # data_bus_capacity_mb comes from template
    assert cfg["data_bus_capacity_mb"] == 5


def test_create_from_template_user_task_appended(
    template_client: TestClient,
    tmp_path: Path,
) -> None:
    """User-provided tasks with new IDs are appended after template tasks."""
    content, filename = _make_user_archive(tmp_path, {
        "agent.json": json.dumps({
            "name": "WithExtraTasks",
            "tasks": [
                {"id": 10, "type": "skill", "description": "user custom skill"},
                {"id": 11, "type": "skill", "description": "another skill"},
            ],
        }),
    })
    resp = template_client.post(
        "/api/packages/create-from-template",
        files={"file": (filename, content, "application/gzip")},
        data={"name": "extra-tasks"},
    )
    assert resp.status_code == 201, resp.text

    get = template_client.get("/api/packages/extra-tasks/file/agent.json")
    assert get.status_code == 200
    cfg = json.loads(get.json()["content"])

    # 4 template tasks + 2 user tasks = 6
    assert len(cfg["tasks"]) == 6
    task_ids = [t["id"] for t in cfg["tasks"]]
    assert task_ids == [0, 1, 2, 3, 10, 11]


def test_create_from_template_rejects_bad_archive(
    template_client: TestClient,
    tmp_path: Path,
) -> None:
    """Unsupported archive format returns 400."""
    bad = tmp_path / "bad.exe"
    bad.write_bytes(b"not an archive")
    resp = template_client.post(
        "/api/packages/create-from-template",
        files={"file": ("bad.exe", bad.read_bytes(), "application/octet-stream")},
        data={"name": "bad-archive"},
    )
    assert resp.status_code == 400
    assert "Unsupported archive format" in resp.text


def test_create_from_template_rejects_unknown_template(
    template_client: TestClient,
    tmp_path: Path,
) -> None:
    """Non-existent template name returns 400 with available list."""
    content, filename = _make_user_archive(tmp_path, {"agent.md": "---\n---"})
    resp = template_client.post(
        "/api/packages/create-from-template",
        files={"file": (filename, content, "application/gzip")},
        data={"name": "no-tmpl", "template": "does_not_exist"},
    )
    assert resp.status_code == 400
    assert "Template 'does_not_exist'" in resp.text
    assert "harness_agent" in resp.text  # Available templates listed


def test_create_from_template_rejects_duplicate_name(
    template_client: TestClient,
    tmp_path: Path,
) -> None:
    """Uploading with a package name that already exists returns 409."""
    content, filename = _make_user_archive(tmp_path, {"agent.md": "---\n---"})

    # First call succeeds
    resp1 = template_client.post(
        "/api/packages/create-from-template",
        files={"file": (filename, content, "application/gzip")},
        data={"name": "dup-test"},
    )
    assert resp1.status_code == 201

    # Second call for same name returns 409
    content2, filename2 = _make_user_archive(tmp_path, {"agent.md": "---\n---"})
    resp2 = template_client.post(
        "/api/packages/create-from-template",
        files={"file": (filename2, content2, "application/gzip")},
        data={"name": "dup-test"},
    )
    assert resp2.status_code == 409
    assert "already exists" in resp2.text


def test_create_from_template_preserves_template_only_files(
    template_client: TestClient,
    tmp_path: Path,
) -> None:
    """Template-owned infra files are restored even if user archive overwrites them."""
    content, filename = _make_user_archive(tmp_path, {
        "agent.md": "---\n---",
        # User tries to overwrite template-owned files
        "nodes/databus_client.py": "# malicious override",
        "harness/tools/bash.py": "# malicious override",
        "tests/test_template.py": "# malicious override",
    })
    resp = template_client.post(
        "/api/packages/create-from-template",
        files={"file": (filename, content, "application/gzip")},
        data={"name": "secure-pkg"},
    )
    assert resp.status_code == 201, resp.text

    # Template files must NOT be user's version
    get = template_client.get("/api/packages/secure-pkg/file/nodes/databus_client.py")
    assert get.status_code == 200
    assert get.json()["content"] == "# template databus"

    get = template_client.get("/api/packages/secure-pkg/file/harness/tools/bash.py")
    assert get.status_code == 200
    assert get.json()["content"] == "# template tool"

    get = template_client.get("/api/packages/secure-pkg/file/tests/test_template.py")
    assert get.status_code == 200
    assert get.json()["content"] == "# template test"

    # User's agent.md should survive (user wins)
    get = template_client.get("/api/packages/secure-pkg/file/agent.md")
    assert get.status_code == 200
    assert "---\n---" in get.json()["content"]


def test_create_from_template_zip_format(
    template_client: TestClient,
    tmp_path: Path,
) -> None:
    """Compressed upload (.zip) also works."""
    content, filename = _make_user_archive(
        tmp_path, {"agent.md": "---\nname: ZipAgent\n---\n"}, fmt="zip",
    )
    resp = template_client.post(
        "/api/packages/create-from-template",
        files={"file": (filename, content, "application/zip")},
        data={"name": "zip-agent"},
    )
    assert resp.status_code == 201, resp.text
    assert resp.json()["name"] == "zip-agent"

    get = template_client.get("/api/packages/zip-agent/file/agent.md")
    assert get.status_code == 200
    assert "ZipAgent" in get.json()["content"]
