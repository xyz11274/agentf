"""Tests for AgentFlow Bridge — Skill Hub REST routes.

Covers the nested skill discovery logic:

* ``GET /api/skill-hub`` lists skills found by recursively scanning for
  ``SKILL.md`` files.
* Nested paths (e.g. ``dashscope_outfit_skills/forge-image-gen``) are
  reported as the skill name.
* Hidden directories (names starting with ``.``) are excluded.
* Directories without ``SKILL.md`` are excluded.
* ``GET /api/skill-hub/{name}`` returns detail for a nested skill.
* ``DELETE /api/skill-hub/{name}`` deletes a nested skill.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

import pytest
from fastapi.testclient import TestClient

import config as config_module
from main import bridge_app as app


# ── Fixtures ────────────────────────────────────────────────────────────────


def _make_skill(skill_dir: Path, name: str, description: str = "") -> None:
    """Create a skill directory with a minimal SKILL.md."""
    skill_dir.mkdir(parents=True, exist_ok=True)
    fm_parts = [f"name: {name}"]
    if description:
        fm_parts.append(f"description: {description}")
    skill_md = "---\n" + "\n".join(fm_parts) + "\n---\n"
    (skill_dir / "SKILL.md").write_text(skill_md)


@pytest.fixture
def fake_skill_hub(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    """Point ``settings.skill_hub_path`` at a temp dir with a nested structure.

    Layout::

        skill_hub/
        ├── dashscope_outfit_skills/
        │   ├── forge-image-gen/   SKILL.md   ← skill
        │   ├── outfit-stylist/    SKILL.md   ← skill
        │   └── video-gen/         SKILL.md   ← skill
        ├── minimax_outfit_skills/
        │   ├── forge-image-gen/   SKILL.md   ← skill
        │   └── outfit-stylist/    SKILL.md   ← skill (video-gen intentionally omitted)
        ├── empty_dir/                        ← no SKILL.md, excluded
        └── .hidden_dir/
            └── some-skill/        SKILL.md   ← excluded (hidden parent)
    """
    root = tmp_path / "skill_hub"
    root.mkdir()

    # Nested dashscope skills
    _make_skill(root / "dashscope_outfit_skills" / "forge-image-gen",
                "forge-image-gen", "DashScope image gen")
    _make_skill(root / "dashscope_outfit_skills" / "outfit-stylist",
                "outfit-stylist", "DashScope outfit stylist")
    _make_skill(root / "dashscope_outfit_skills" / "video-gen",
                "video-gen", "DashScope video gen")

    # Nested minimax skills (one fewer — tests that missing ones don't appear)
    _make_skill(root / "minimax_outfit_skills" / "forge-image-gen",
                "forge-image-gen", "MiniMax image gen")
    _make_skill(root / "minimax_outfit_skills" / "outfit-stylist",
                "outfit-stylist", "MiniMax outfit stylist")

    # Empty directory — no SKILL.md, should be excluded
    (root / "empty_dir").mkdir()

    # Hidden directory — should be excluded
    (root / ".hidden_dir" / "some-skill").mkdir(parents=True)
    (root / ".hidden_dir" / "some-skill" / "SKILL.md").write_text("---\nname: some-skill\n---\n")

    monkeypatch.setattr(config_module.settings, "skill_hub_dir", str(root))
    return root


@pytest.fixture
def client(fake_skill_hub: Path) -> TestClient:
    return TestClient(app)


# ── Tests: GET /api/skill-hub (list) ──────────────────────────────────────


class TestListSkills:
    """``GET /api/skill-hub`` — recursive nested discovery."""

    def test_lists_all_nested_skills(self, client: TestClient):
        resp = client.get("/api/skill-hub")
        assert resp.status_code == 200
        data: list[dict[str, Any]] = resp.json()

        names = sorted(s["name"] for s in data)
        assert names == [
            "dashscope_outfit_skills/forge-image-gen",
            "dashscope_outfit_skills/outfit-stylist",
            "dashscope_outfit_skills/video-gen",
            "minimax_outfit_skills/forge-image-gen",
            "minimax_outfit_skills/outfit-stylist",
        ]

    def test_excludes_hidden_directories(self, client: TestClient):
        """Directories whose path contains a '.'-prefixed segment are excluded."""
        resp = client.get("/api/skill-hub")
        assert resp.status_code == 200
        names = [s["name"] for s in resp.json()]
        assert not any("hidden" in n or n.startswith(".") for n in names)
        assert "some-skill" not in names

    def test_excludes_dirs_without_skilLmd(self, client: TestClient):
        """Directories without SKILL.md do not appear in the list."""
        resp = client.get("/api/skill-hub")
        assert resp.status_code == 200
        names = [s["name"] for s in resp.json()]
        assert "empty_dir" not in names

    def test_skill_description_from_frontmatter(self, client: TestClient):
        resp = client.get("/api/skill-hub")
        assert resp.status_code == 200
        data: list[dict[str, Any]] = resp.json()
        by_name = {s["name"]: s for s in data}

        fs = by_name["dashscope_outfit_skills/forge-image-gen"]
        assert fs["description"] == "DashScope image gen"
        assert fs["version"] == "1.0.0"

        ms = by_name["minimax_outfit_skills/outfit-stylist"]
        assert ms["description"] == "MiniMax outfit stylist"

    def test_returns_empty_when_hub_dir_does_not_exist(
        self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
    ):
        missing = tmp_path / "i_do_not_exist"
        monkeypatch.setattr(config_module.settings, "skill_hub_dir", str(missing))
        resp = TestClient(app).get("/api/skill-hub")
        assert resp.status_code == 200
        assert resp.json() == []

    def test_returns_empty_when_no_skilLmd_files(
        self, tmp_path: Path, monkeypatch: pytest.MonkeyPatch
    ):
        root = tmp_path / "empty_hub"
        root.mkdir()
        (root / "some_dir").mkdir()  # directory without SKILL.md
        monkeypatch.setattr(config_module.settings, "skill_hub_dir", str(root))
        resp = TestClient(app).get("/api/skill-hub")
        assert resp.status_code == 200
        assert resp.json() == []


# ── Tests: GET /api/skill-hub/{name} (detail) ──────────────────────────────


class TestGetSkill:
    """``GET /api/skill-hub/{name}`` — detail with full SKILL.md content."""

    def test_get_nested_skill(self, client: TestClient):
        resp = client.get("/api/skill-hub/dashscope_outfit_skills/forge-image-gen")
        assert resp.status_code == 200
        data = resp.json()
        assert data["name"] == "dashscope_outfit_skills/forge-image-gen"
        assert data["description"] == "DashScope image gen"
        # Content is body-only (frontmatter stripped)
        assert "content" in data

    def test_404_for_nonexistent_nested_skill(self, client: TestClient):
        resp = client.get("/api/skill-hub/minimax_outfit_skills/video-gen")
        assert resp.status_code == 404

    def test_404_for_flat_name_that_is_not_a_skill(self, client: TestClient):
        resp = client.get("/api/skill-hub/forge-image-gen")
        assert resp.status_code == 404

    def test_403_for_path_traversal(self, client: TestClient):
        # FastAPI normalizes ``..`` before routing, so the path resolves to
        # a non-existent route (404).  The ``_validate_skill_name`` ``..``
        # check is defense-in-depth — it is hit when the path arrives via
        # internal calls (e.g. install_skill), not through the HTTP router.
        resp = client.get("/api/skill-hub/../config.py")
        assert resp.status_code == 404

    def test_403_for_name_with_special_chars(self, client: TestClient):
        resp = client.get("/api/skill-hub/skill<script>")
        assert resp.status_code == 403


# ── Tests: DELETE /api/skill-hub/{name} ────────────────────────────────────


class TestDeleteSkill:
    """``DELETE /api/skill-hub/{name}`` — delete a nested skill."""

    def test_delete_nested_skill(self, client: TestClient, fake_skill_hub: Path):
        resp = client.delete(
            "/api/skill-hub/dashscope_outfit_skills/video-gen"
        )
        assert resp.status_code == 204

        # Confirm it's gone from listing
        resp2 = client.get("/api/skill-hub")
        names = [s["name"] for s in resp2.json()]
        assert "dashscope_outfit_skills/video-gen" not in names

    def test_delete_returns_404_for_nonexistent(self, client: TestClient):
        resp = client.delete("/api/skill-hub/nonexistent/skill")
        assert resp.status_code == 404

    def test_delete_returns_403_for_path_traversal(self, client: TestClient):
        # FastAPI normalizes ``..`` before routing. See test_403_for_path_traversal.
        resp = client.delete("/api/skill-hub/../config.py")
        assert resp.status_code == 404
