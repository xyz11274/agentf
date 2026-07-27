"""Template merge engine — merges a harness agent template with user-uploaded content.

Merge strategy (high-level):
─────────────────────────────
1. Copy template → target directory (full scaffolding)
2. Rewrite ``working_dir`` in workflow JSONs to match the new package path
3. Extract user's archive into the target on top (user files win for direct conflicts)
4. Deep-merge ``agent.json`` — preserve template's infrastructure tasks, let user
   override top-level identity fields (name, version, description, pool sizes).
5. Ensure template-owned infrastructure files always stay intact.

Ownership rules (see :data:`_TEMPLATE_ONLY` and :data:`_USER_WINS`):

| Category            | Winner   | Examples                                     |
|---------------------|----------|----------------------------------------------|
| Runtime infra       | Template | ``harness/tools/*``, ``nodes/databus*``, …   |
| User identity       | User     | ``agent.md``, ``package.json``               |
| Smart merge         | Deep     | ``agent.json`` (tasks kept + user top-level) |
| Workflow DAGs       | Tmpl+rw  | ``coordinator_workflow.json``, … (rewrite wd)|
| User additive       | User+    | Custom ``harness/skills/<custom>/``, ``data/``|

Design doc: ``docs/design/features/agent-template-merge.md``
"""

from __future__ import annotations

import json
import logging
import shutil
import tarfile
import tempfile
import zipfile
from pathlib import Path
from typing import Any

logger = logging.getLogger("agentflow.bridge.merger")

# ---------------------------------------------------------------------------
# Constants — file ownership rules
# ---------------------------------------------------------------------------

# Template-owned: absolute relative paths that the template always clobbers.
# User-uploaded files matching these patterns are discarded at merge time so
# the runtime infrastructure cannot be accidentally broken.
_TEMPLATE_ONLY: tuple[str, ...] = (
    # Runtime tools — critical infrastructure
    "harness/tools/",
    "harness/skills/skill-runner/",
    # dummy_pre_skill 的 config_dir 不能存在，否则 MMAgent 将 workflow_file 解析为相对路径
    # 导致找不到文件。由 _build_agent_from_src 在 merger 完成后删除。
    # "harness/skills/dummy_pre_skill/",
    "dummy_pre_skill_workflow.json",
    "nodes/dummy_pre_skill.sh",
    "nodes/dummy_pre_skill.py",
    # DataBus + LLM — core nodes
    "nodes/databus_client.py",
    "nodes/databus_client.pyc",
    "nodes/llm_call.py",
    "nodes/llm_call.pyc",
    "nodes/dummy_pre_skill.py",
    "nodes/dummy_pre_skill.sh",
    "nodes/parse_af_input.py",
    # Shell wrappers — framework infrastructure (§3.5 standard implementation)
    # Template always clobbers so wrapper venv-detection logic stays in sync
    "nodes/coordinator_decide.sh",
    "nodes/skill_executor.sh",
    "nodes/dummy_pre_skill.sh",
    # Workflow DAGs — framework infrastructure (execution_mode + command declaration)
    # Template always clobbers so shell_script/wrapper 范式 stays in sync
    "coordinator_workflow.json",
    "skill_workflow.json",
    "dummy_pre_skill_workflow.json",
    # setup.sh — framework infrastructure (venv + sentinel contract §3.5)
    "setup.sh",
    # Tests — always template-owned
    "tests/",
)

# Workflow files whose ``working_dir`` must be rewritten.
_WORKFLOW_FILES: tuple[str, ...] = (
    "coordinator_workflow.json",
    "skill_workflow.json",
    "dummy_pre_skill_workflow.json",
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _copy_template(template_dir: Path, target_dir: Path) -> None:
    """Recursively copy *template_dir* → *target_dir* (creating parents)."""
    target_dir.mkdir(parents=True, exist_ok=True)
    for entry in template_dir.iterdir():
        dst = target_dir / entry.name
        if entry.is_dir():
            shutil.copytree(entry, dst, dirs_exist_ok=True, symlinks=True)
        else:
            shutil.copy2(entry, dst)


def _rewrite_working_dir(target_dir: Path, template_workdir: str,
                         new_workdir: str) -> None:
    """Rewrite ``working_dir`` in every workflow JSON under *target_dir*."""
    for wf_name in _WORKFLOW_FILES:
        wf_path = target_dir / wf_name
        if not wf_path.exists():
            continue
        content = wf_path.read_text(encoding="utf-8")
        # Replace ALL occurrences (one per node) of the old workdir
        updated = content.replace(template_workdir, new_workdir)
        if updated != content:
            logger.info(
                "rewrite working_dir %s → %s in %s",
                template_workdir, new_workdir, wf_name,
            )
            wf_path.write_text(updated, encoding="utf-8")


def _deep_merge_agent_json(
    target_path: Path,
    template_agent_json: dict[str, Any] | None,
    user_agent_json: dict[str, Any] | None,
) -> None:
    """Deep-merge user's ``agent.json`` on top of template's.

    Template tasks array is ALWAYS preserved (infrastructure). User overrides
    top-level fields (name, version, description, pool sizes, …). If user
    provides extra tasks (with IDs not conflicting with template tasks), they
    are appended after the template tasks.
    """
    if not target_path.exists():
        return

    if not template_agent_json and not user_agent_json:
        return

    # Start with template as the structural base
    base: dict[str, Any] = {}
    if template_agent_json:
        # Deep-copy so we don't mutate the caller's dict
        base = json.loads(json.dumps(template_agent_json))

    if not user_agent_json:
        target_path.write_text(
            json.dumps(base, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        return

    # --- top-level fields: user overrides template -------------------------
    _USER_OVERRIDE_KEYS = {
        "name", "version", "description",
        "session_pool_size", "max_workflows",
        "max_concurrent_workflows",
    }
    for key in _USER_OVERRIDE_KEYS:
        if key in user_agent_json:
            base[key] = user_agent_json[key]

    # --- tasks: preserve template tasks, append user-provided ones ---------
    template_tasks: list[dict[str, Any]] = base.get("tasks", [])
    template_task_ids = {t["id"] for t in template_tasks if "id" in t}
    user_tasks: list[dict[str, Any]] = user_agent_json.get("tasks", [])

    next_task_id = max(template_task_ids, default=-1) + 1

    for ut in user_tasks:
        tid = ut.get("id")
        if tid is None:
            ut["id"] = next_task_id
            next_task_id += 1
            template_tasks.append(ut)
        elif tid not in template_task_ids:
            template_task_ids.add(tid)
            template_tasks.append(ut)
            next_task_id = max(next_task_id, tid + 1)
        # If the task ID already exists in template, skip (template wins for tasks)

    base["tasks"] = template_tasks

    # --- write merged result back ------------------------------------------
    target_path.write_text(
        json.dumps(base, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    logger.info(
        "agent.json merged: override-keys=%s user-tasks-kept=%d",
        [k for k in _USER_OVERRIDE_KEYS if k in user_agent_json],
        len(user_tasks),
    )


def _extract_archive(archive_path: Path, target_dir: Path, is_tar: bool) -> None:
    """Extract *archive_path* into *target_dir*.

    File-level: user files overwrite template files. Template-owned
    infrastructure is re-clobbered after extraction (``_reapply_template_files``).

    Both tar and zip paths include path-traversal protection: entries using
    absolute paths or ``../`` that escape *target_dir* are rejected.
    """
    if is_tar:
        with tarfile.open(archive_path, "r:gz") as tf:
            tf.extractall(path=target_dir, filter="data")
    else:
        with zipfile.ZipFile(archive_path, "r") as zf:
            resolved_target = target_dir.resolve()
            for entry in zf.infolist():
                dest = (resolved_target / entry.filename).resolve()
                if not str(dest).startswith(str(resolved_target) + "/"):
                    raise ValueError(
                        f"Zip entry '{entry.filename}' escapes target directory"
                    )
                zf.extract(entry, resolved_target)


def _reapply_template_files(template_dir: Path, target_dir: Path) -> None:
    """Re-copy all template-owned infrastructure files over the target.

    This guarantees that even if the user archive overwrote a critical
    infrastructure file, the template version is restored.
    """
    # Sanity check: warn if template has infrastructure files not in _TEMPLATE_ONLY.
    # This prevents silent "update won't sync" drift when new files are added to
    # template without being listed. Only scans nodes/ (the most common miss).
    _check_template_only_coverage(template_dir)

    for prefix in _TEMPLATE_ONLY:
        src = template_dir / prefix
        dst = target_dir / prefix
        if not src.exists():
            continue
        if src.is_dir():
            shutil.copytree(src, dst, dirs_exist_ok=True, symlinks=True)
        else:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)


def _check_template_only_coverage(template_dir: Path) -> None:
    """Warn if template has infrastructure files not covered by _TEMPLATE_ONLY.

    Scans ``nodes/`` for ``*.sh``, ``*.py``, ``*.json`` — the most common
    miss path. Only warns (does not block), because a missing entry only
    causes "update won't sync that file" — existing functionality is intact.
    """
    _expected_exts = ("*.sh", "*.py", "*.json")
    for ext in _expected_exts:
        for f in (template_dir / "nodes").glob(ext):
            # 构造 _TEMPLATE_ONLY 格式的 prefix（如 "nodes/xxx.sh"）
            prefix = str(Path("nodes") / f.name)
            if prefix in _TEMPLATE_ONLY:
                continue
            # 如果 nodes/ 整体被目录覆盖也跳过（当前没有，但防御性）
            if any(p.endswith("/") and p.startswith("nodes/") for p in _TEMPLATE_ONLY):
                continue
            logger.warning(
                "template_only.coverage_miss: %s not in _TEMPLATE_ONLY "
                "(update won't sync this file to existing agents)",
                prefix,
            )


def _make_scripts_executable(pkg_dir: Path) -> None:
    """Restore execute permission on shell scripts and Python entry files."""
    for p in pkg_dir.rglob("*.sh"):
        st = p.stat()
        p.chmod(st.st_mode | 0o111)
    for p in pkg_dir.rglob("*.py"):
        st = p.stat()
        p.chmod(st.st_mode | 0o111)


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------


def merge_template_with_user_upload(
    template_dir: Path,
    user_archive: bytes,
    archive_filename: str,
    package_name: str,
    target_dir: Path,
    package_workdir: str,
) -> None:
    """Merge *template_dir* with user's uploaded archive into *target_dir*.

    Parameters
    ----------
    template_dir:
        Full path to the template (e.g. ``templates/harness_agent/``).
    user_archive:
        Raw bytes of the uploaded archive (.zip / .tar.gz).
    archive_filename:
        Original filename for format detection.
    package_name:
        Target package name (e.g. ``my-cool-agent``).
    target_dir:
        Directory where the merged package will be written (created if needed).
    package_workdir:
        Relative ``working_dir`` for workflow files, from project root
        (e.g. ``examples/agent/my-cool-agent``).

    Raises
    ------
    ValueError
        Unsupported archive format.
    OSError
        Filesystem errors (permissions, disk full, …).
    """
    lower_name = archive_filename.lower()
    is_tar = lower_name.endswith((".tar.gz", ".tgz"))
    is_zip = lower_name.endswith(".zip")

    if not is_tar and not is_zip:
        raise ValueError(
            f"Unsupported archive format: {archive_filename!r}. "
            f"Allowed: .zip, .tar.gz, .tgz"
        )

    # --- Phase 0: snapshot template's original agent.json before any copy ---
    template_agent_json: dict[str, Any] | None = None
    template_agent_path = template_dir / "agent.json"
    if template_agent_path.exists():
        try:
            template_agent_json = json.loads(
                template_agent_path.read_text(encoding="utf-8")
            )
        except (json.JSONDecodeError, OSError) as exc:
            logger.warning("Failed to read template agent.json: %s", exc)

    # --- Phase 1: full template copy → target -----------------------------
    logger.info(
        "merge_start pkg=%s template=%s archive=%s",
        package_name, template_dir.name, archive_filename,
    )
    _copy_template(template_dir, target_dir)

    # --- Phase 3: extract user archive on top, re-apply infra -------------
    suffix = ".tar.gz" if is_tar else ".zip"
    with tempfile.NamedTemporaryFile(delete=False, suffix=suffix) as tmp:
        tmp_path = Path(tmp.name)
        tmp.write(user_archive)

    try:
        _extract_archive(tmp_path, target_dir, is_tar)
        # Re-clobber template-owned infrastructure files
        _reapply_template_files(template_dir, target_dir)
    finally:
        tmp_path.unlink(missing_ok=True)

    # --- Phase 2 (after reapply): rewrite working_dir in workflow files ---
    # MUST be after _reapply_template_files, which overwrites workflow files
    # from the template (including the template's original working_dir).
    template_workdir = _detect_template_workdir(template_dir)
    if template_workdir:
        _rewrite_working_dir(target_dir, template_workdir, package_workdir)

    # --- Phase 4: deep-merge agent.json -----------------------------------
    # Read whatever agent.json is on disk now (could be user's or template's)
    current_agent_json: dict[str, Any] | None = None
    current_agent_path = target_dir / "agent.json"
    if current_agent_path.exists():
        try:
            current_agent_json = json.loads(
                current_agent_path.read_text(encoding="utf-8")
            )
        except (json.JSONDecodeError, OSError) as exc:
            logger.warning("Failed to read merged agent.json: %s", exc)

    _deep_merge_agent_json(
        target_path=current_agent_path,
        template_agent_json=template_agent_json,
        user_agent_json=current_agent_json,
    )

    # --- Phase 5: restore execute bits -------------------------------------
    _make_scripts_executable(target_dir)

    file_count = sum(1 for p in target_dir.rglob("*") if p.is_file())
    logger.info(
        "merge_complete pkg=%s files=%d template_only=%s",
        package_name, file_count,
        [p.rstrip("/") for p in _TEMPLATE_ONLY],
    )


def _detect_template_workdir(template_dir: Path) -> str | None:
    """Read the first workflow file to extract its original ``working_dir``."""
    for wf_name in _WORKFLOW_FILES:
        wf_path = template_dir / wf_name
        if wf_path.exists():
            try:
                data = json.loads(wf_path.read_text(encoding="utf-8"))
                nodes = data.get("nodes", [])
                for node in nodes:
                    wd = node.get("working_dir", "")
                    if wd:
                        return wd
            except (json.JSONDecodeError, OSError):
                continue
    return None

