"""Harness tools package — auto-discovers and registers all tool modules."""
import os
import importlib

_TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))

# Auto-import all .py files in this directory (excluding __init__.py)
for f in os.listdir(_TOOLS_DIR):
    if f.endswith(".py") and f != "__init__.py":
        module_name = f[:-3]
        try:
            importlib.import_module(f".{module_name}", __package__)
        except Exception:
            pass
