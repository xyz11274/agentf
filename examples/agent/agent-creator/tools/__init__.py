"""tools package — ensures tools directory is on sys.path for file_store imports.

This module only sets up the import path. Tool discovery and registration
is handled by ``llm_loop.py:_discover_tools()`` — the single authority for
building the ``tool_map`` used by the LLM loop.
"""
import os
import sys

_TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))

# Allow bare "from file_store import files" in tool modules
if _TOOLS_DIR not in sys.path:
    sys.path.insert(0, _TOOLS_DIR)
