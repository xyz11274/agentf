#!/usr/bin/env python3
"""Generate entry files for an agent package.

Entry files are thin wrappers (1 line import) that allow daemon workflow
execution to find the entry point, while the actual implementation lives
in pip packages (agentflow-runtime, agentflow-adapter).

Usage:
    python3 scripts/generate-entry-files.py <agent_package_dir>

Example:
    python3 scripts/generate-entry-files.py examples/agent/outfit-assistant
"""

from __future__ import annotations

import os
import sys
from pathlib import Path


# ── Entry file mapping:  agent_path → package_import ────────────────
# Only files with ``if __name__ == "__main__":`` need explicit function
# imports (``execute``).  Library modules just need ``import``.
ENTRY_FILES: dict[str, str] = {
    # ── Daemon workflow entry points (only these need files for execve) ──
    # Library modules are imported via absolute ``from agentflow_runtime.xxx``
    # paths, so they don't need entry files in the node directory.
    "nodes/coordinator_decide.py":
        "import sys as _sys\nfrom agentflow_runtime import coordinator_decide as _m; import os as _os; _m.__dict__['AGENT_PACKAGE_DIR'] = _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))); _m.__dict__['AGENT_MD_PATH'] = _os.path.join(_m.__dict__['AGENT_PACKAGE_DIR'], 'agent.md'); _m.__dict__['NODES_DIR'] = _os.path.dirname(_os.path.abspath(__file__)); _is_main = __name__ == '__main__'; globals().update(_m.__dict__)\nif _is_main:\n    execute()",

    "nodes/skill_executor.py":
        "import sys as _sys\nfrom agentflow_runtime import skill_executor as _m; import os as _os; _m.__dict__['AGENT_PACKAGE_DIR'] = _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))); _m.__dict__['AGENT_MD_PATH'] = _os.path.join(_m.__dict__['AGENT_PACKAGE_DIR'], 'agent.md'); _m.__dict__['NODES_DIR'] = _os.path.dirname(_os.path.abspath(__file__)); _is_main = __name__ == '__main__'; globals().update(_m.__dict__)\nif _is_main:\n    execute()",

    "nodes/dummy_pre_skill.py":
        "from agentflow_runtime import dummy_pre_skill as _m; globals().update(_m.__dict__)",
}


def generate_entry_files(agent_dir: str) -> list[str]:
    """Generate entry files in the given agent package directory.

    Returns a list of generated file paths.
    """
    agent_path = Path(agent_dir).resolve()
    generated: list[str] = []

    for rel_path, import_line in ENTRY_FILES.items():
        target = agent_path / rel_path
        target.parent.mkdir(parents=True, exist_ok=True)

        # Don't overwrite existing files that are NOT entry files
        # (i.e., files that have more than 5 lines of code)
        if target.exists():
            content = target.read_text(encoding="utf-8")
            force = os.environ.get("ENTRY_FILE_FORCE") == "1"
            if len(content.splitlines()) > 5 and not force:
                print(f"  ⏭️  {rel_path} (already exists, >5 lines, skipping)")
                continue

        target.write_text(
            f"# Auto-generated entry file — do not modify\n"
            f"# The actual implementation lives in the pip package.\n"
            f"{import_line}\n",
            encoding="utf-8",
        )
        generated.append(str(target))
        print(f"  ✅ {rel_path} → {import_line}")

    return generated


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Generate entry files for an agent package")
    parser.add_argument("agent_dir", help="Agent package directory")
    parser.add_argument("--force", action="store_true", help="Overwrite existing files")
    args = parser.parse_args()

    if not os.path.isdir(args.agent_dir):
        print(f"Error: '{args.agent_dir}' is not a directory")
        sys.exit(1)

    print(f"Generating entry files for: {args.agent_dir}")
    print(f"Source: agentflow-runtime (pip) + agentflow-adapter (pip)")
    if args.force:
        print("Mode: force overwrite")
    print()

    # Pass --force to generate_entry_files via a global or env
    if args.force:
        os.environ["ENTRY_FILE_FORCE"] = "1"
    generated = generate_entry_files(args.agent_dir)
    os.environ.pop("ENTRY_FILE_FORCE", None)

    print()
    print(f"Generated {len(generated)} entry files.")


if __name__ == "__main__":
    main()