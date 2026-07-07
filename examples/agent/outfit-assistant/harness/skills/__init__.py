"""Harness skills package — auto-discovers all skill modules.

Each skill is a subdirectory under ``skills/`` containing:
  - ``SKILL.md`` — skill definition and usage
  - ``scripts/`` — executable Python modules with TOOL_NAME/TOOL_PARAMS/execute()

Scripts are discovered at import time and their attributes (TOOL_NAME etc.)
are re-exported at package level so ``llm_loop.py`` can find them via ``dir()``.
"""
import importlib
import os
import sys

_SKILLS_DIR = os.path.dirname(os.path.abspath(__file__))
_SKILLS_PKG = __package__

for entry in sorted(os.listdir(_SKILLS_DIR)):
    entry_path = os.path.join(_SKILLS_DIR, entry)
    scripts_dir = os.path.join(entry_path, "scripts")
    if os.path.isdir(scripts_dir):
        for f in sorted(os.listdir(scripts_dir)):
            if f.endswith(".py") and f != "__init__.py":
                module_name = f[:-3]
                full_module = f"{_SKILLS_PKG}.{entry}.scripts.{module_name}"
                try:
                    mod = importlib.import_module(full_module, __package__)
                    for attr in dir(mod):
                        if not attr.startswith("_"):
                            setattr(sys.modules[_SKILLS_PKG], attr, getattr(mod, attr))
                except Exception:
                    pass
