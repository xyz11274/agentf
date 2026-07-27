"""Generate & load ``agent-package.manifest.jsonc`` for E2B sandbox upload (T-004).

Scanning rules (§3.2):
  ✅ ``harness/skills/**``       — always included if exists
  ✅ ``harness/tools/**``        — included if exists (user overrides)
  ✅ ``data/**``                 — included if exists
  ✅ ``requirements.txt``        — included if exists
  ❌ ``agent.json``, ``package.json``, ``coordinator*_workflow.json``,
     ``skill_workflow.json``, ``nodes/``, ``agent.md``, ``setup.sh``, ``.env``
"""

import json
import os
from datetime import datetime, timezone
from pathlib import Path

MANIFEST_NAME = "agent-package.manifest.jsonc"

# Globs to include (relative to agent_package_dir)
_INCLUDE_GLOBS = [
    "harness/skills/**",
    "harness/tools/**",  # user overrides
    "data/**",
    "requirements.txt",
]

# Files/directories to exclude even if they match a glob
_EXCLUDE_PREFIXES = ("agent.json", "package.json", "coordinator")

# File names to exclude (top-level only)
_EXCLUDE_NAMES = {"agent.md", "setup.sh", ".env"}

# Directories to exclude entirely
_EXCLUDE_DIRS = {"nodes", "__pycache__"}


def generate_manifest(agent_package_dir: str | os.PathLike) -> dict:
    """Generate (or reload) ``agent-package.manifest.jsonc`` for the package.

    If the manifest file already exists, returns its content without rescanning.

    Returns:
        Parsed manifest dict with keys ``files`` (list of ``{"path": ...}``),
        ``generated_at`` (ISO 8601 timestamp).
    """
    base = Path(agent_package_dir)
    manifest_path = base / MANIFEST_NAME

    # If manifest already exists, return it directly
    if manifest_path.exists():
        return load_manifest(base) or _rescan_and_write(base, manifest_path)

    return _rescan_and_write(base, manifest_path)


def load_manifest(agent_package_dir: str | os.PathLike) -> dict | None:
    """Load an existing manifest file, or return ``None``.

    Returns ``None`` if the file doesn't exist or is unparseable.
    """
    path = Path(agent_package_dir) / MANIFEST_NAME
    if not path.exists():
        return None
    try:
        raw = path.read_text(encoding="utf-8")
        return json.loads(raw)
    except (json.JSONDecodeError, OSError):
        return None


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------


def _rescan_and_write(base: Path, manifest_path: Path) -> dict:
    """Scan the package directory and write a fresh manifest."""
    files = []

    for entry in base.iterdir():
        name = entry.name

        # Skip excluded names and dirs
        if name in _EXCLUDE_NAMES:
            continue
        if name in _EXCLUDE_DIRS:
            continue
        if name.startswith(_EXCLUDE_PREFIXES):
            continue

        if name == "harness":
            _scan_harness(entry, base, files)
        elif name == "data":
            _scan_recursive(entry, base, files)
        elif name == "requirements.txt" and entry.is_file():
            files.append({"path": "requirements.txt"})

    manifest = {
        "files": files,
        "generated_at": datetime.now(timezone.utc).isoformat(),
    }
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")
    return manifest


def _scan_harness(harness_dir: Path, base: Path, files: list) -> None:
    """Scan harness/ — include skills/ and tools/, exclude everything else."""
    skills_dir = harness_dir / "skills"
    if skills_dir.is_dir():
        _scan_recursive(skills_dir, base, files)

    tools_dir = harness_dir / "tools"
    if tools_dir.is_dir():
        _scan_recursive(tools_dir, base, files)


def _scan_recursive(dir_path: Path, base: Path, files: list) -> None:
    """Recursively collect all regular files under a directory."""
    for entry in dir_path.rglob("*"):
        if entry.is_file():
            rel = entry.relative_to(base).as_posix()
            files.append({"path": rel})
