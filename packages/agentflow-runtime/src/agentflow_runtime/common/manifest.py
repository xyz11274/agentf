"""Generate & load ``manifest.jsonc`` for e2b sandbox upload (T-004).

Scanning rules (§3.2):
  ✅ ``harness/skills/**``       — always included if exists
  ✅ ``harness/tools/**``        — included if exists (user overrides)
  ✅ ``data/**``                 — included if exists
  ✅ ``.env``                    — top-level .env file (keys: DASHSCOPE_API_KEY etc)
  ✅ ``setup.sh``                — top-level only, environment init script
  ✅ ``requirements.txt``        — included if exists
  ❌ ``agent.json``, ``package.json``, ``coordinator*_workflow.json``,
     ``skill_workflow.json``, ``nodes/``, ``agent.md``
"""

import json
import os
from datetime import datetime, timezone
from pathlib import Path

MANIFEST_NAME = "manifest.jsonc"

# Globs to include (relative to agent_package_dir)
# 系统预制 includes——用户可在 manifest.jsonc 的 includes 键中覆盖/扩展
_INCLUDE_GLOBS = [
    "harness/skills/**",
    "harness/tools/**",  # user overrides
    "data/**",
    "requirements.txt",
    ".env",        # 密钥文件——sandbox-env-injection.md §6.1 set -a && source 链路要求（只传主 .env）
    "setup.sh",    # 沙箱内环境初始化脚本
]

# Files/directories to exclude even if they match a glob
_EXCLUDE_PREFIXES = ("agent.json", "package.json", "coordinator")

# File names to exclude (top-level only)
# 注：.env 与 setup.sh 曾在此处排除，但 sandbox-env-injection.md §6.1 敲定的
# set -a && source /home/user/workspace/.env 链路要求 .env 必须在沙箱内（本轮修订，
# 2026-08-04）。setup.sh 是沙箱内环境初始化脚本，随包上传由框架执行。
# 修订来源：docs/design/features/agent-package/agent-package-structure.md §3.2 上传规则（.env / setup.sh 纳入 includes）
_EXCLUDE_NAMES = {"agent.md"}

# Directories to exclude entirely
# nodes/ = daemon 侧执行不上传；__pycache__/.venv 等 = 缓存/虚拟环境不上传；
# workspace/、outputs/、output/ = 会话级产物与运行时数据，沙箱侧会重新生成（上传是污染）；
# tests/ = 测试代码，沙箱不跑；.agentfoundry/ = IDE/框架资产管理目录
_EXCLUDE_DIRS = {
    "nodes", "__pycache__", ".venv", "workspace",
    "outputs", "output", "tests", ".agentfoundry",
}

# Cache/产物文件后缀，递归扫描时跳过
_CACHE_SUFFIXES = {".pyc", ".pyo", ".ds_store"}


def generate_manifest(agent_package_dir: str | os.PathLike) -> dict:
    """Generate (or reload) ``manifest.jsonc`` for the package.

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

    manifest.jsonc 是 JSONC 格式（用户可写 ``//`` 注释，见 agent-package-structure.md §3.3），
    解析前剥离注释——不能用裸 ``json.loads``（对带注释文件抛 JSONDecodeError）。
    """
    path = Path(agent_package_dir) / MANIFEST_NAME
    if not path.exists():
        return None
    try:
        raw = path.read_text(encoding="utf-8")
        return json.loads(strip_jsonc_comments(raw))
    except (json.JSONDecodeError, OSError):
        return None


def strip_jsonc_comments(text: str) -> str:
    """Strip JSONC comments (``//`` line and ``/* */`` block) without touching strings.

    JSONC 允许注释（vs 严格 JSON）——manifest.jsonc 由用户手写维护，注释是合法内容。
    状态机逐字符扫描：字符串内的 ``//``/``/*`` 是字面量，不剥离。
    """
    out = []
    i = 0
    n = len(text)
    in_str = False
    in_block = False
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if in_block:
            if ch == "*" and nxt == "/":
                in_block = False
                i += 2
                continue
            i += 1
            continue
        if in_str:
            out.append(ch)
            if ch == "\\":
                if i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
            elif ch == '"':
                in_str = False
            i += 1
            continue
        if ch == '"':
            in_str = True
            out.append(ch)
            i += 1
            continue
        if ch == "/" and nxt == "/":
            # 行注释：跳到行尾
            while i < n and text[i] != "\n":
                i += 1
            continue
        if ch == "/" and nxt == "*":
            in_block = True
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


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
        elif entry.is_file() and (name == ".env" or name == "setup.sh"):
            # .env（主密钥文件，不含变体）与 setup.sh 随包上传——
            # sandbox-env-injection.md §6.1 的 set -a && source /home/user/workspace/.env
            # 链路要求密钥文件在沙箱内；setup.sh 由框架在沙箱内执行环境初始化。
            # 修订来源：agent-package-structure.md §3.2 上传规则（本轮将 .env / setup.sh 纳入 includes；
            # 用户确认只传主 .env，不传 .env_dashscope/.env_minimax 等变体）
            files.append({"path": name})

    manifest = {
        # 系统预制 includes——用户可编辑此键覆盖/扩展上传范围（agent-package-structure.md §3.3）
        "includes": list(_INCLUDE_GLOBS),
        # 系统预制 excludes——用户可编辑此键追加排除项
        "excludes": sorted(_EXCLUDE_DIRS),
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
    """Recursively collect all regular files under a directory, skipping cache dirs."""
    for entry in dir_path.rglob("*"):
        if entry.is_file():
            rel = entry.relative_to(base)
            # 跳过缓存目录（__pycache__、.pytest_cache 等）与 .venv 等无关目录
            if any(part in _EXCLUDE_DIRS for part in rel.parts):
                continue
            if rel.suffix.lower() in _CACHE_SUFFIXES:
                continue
            files.append({"path": rel.as_posix()})
