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
# 注：.env 与 setup.sh 曾在此处排除，但 sandbox-env-injection.md §6.1 敲定的
# set -a && source /home/user/workspace/.env 链路要求 .env 必须在沙箱内（本轮修订，
# 2026-08-04）。setup.sh 是沙箱内环境初始化脚本，随包上传由框架执行。
# 修订来源：docs/design/features/agent-package/agent-package-structure.md §3.2 上传规则（.env / setup.sh 纳入 includes）

# Directories to exclude entirely
# nodes/ = daemon 侧执行不上传；__pycache__/.pytest_cache/.mypy_cache/.ruff_cache 等 = 缓存/虚拟环境不上传；
# .git = 版本控制元数据不上传（与 e2b_sync._CACHE_DIRS 对齐，I-5 单一真相）；
# workspace/、outputs/、output/ = 会话级产物与运行时数据，沙箱侧会重新生成（上传是污染）；
# tests/ = 测试代码，沙箱不跑；.agentfoundry/ = IDE/框架资产管理目录
_EXCLUDE_DIRS = {
    "nodes", "__pycache__", ".pytest_cache", ".mypy_cache", ".ruff_cache", ".git",
    ".venv", "workspace", "outputs", "output", "tests", ".agentfoundry",
}


def generate_manifest(agent_package_dir: str | os.PathLike) -> dict:
    """Generate (or reload) ``manifest.jsonc`` for the package.

    If the manifest file already exists, returns its content without rescanning.

    Returns:
        Parsed manifest dict with keys ``includes`` (glob whitelist),
        ``excludes`` (glob blacklist) and ``generated_at`` (ISO 8601 timestamp).
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
    """Write a fresh manifest with the system-default includes/excludes globs.

    收敛（2026-08-11）：不再扫描并展开具体文件列表（files 键）——includes glob
    由消费端（e2b_sync._load_manifest_whitelist）实时展开，新增文件自动进入
    上传白名单与快照 hash，消除 "files 快照过期" 断点（I-5 单一真相来源）。
    """
    manifest = {
        # 系统预制 includes——用户可编辑此键覆盖/扩展上传范围（agent-package-structure.md §3.3）
        "includes": list(_INCLUDE_GLOBS),
        # 系统预制 excludes——用户可编辑此键追加排除项
        "excludes": sorted(_EXCLUDE_DIRS),
        "generated_at": datetime.now(timezone.utc).isoformat(),
    }
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")
    return manifest
