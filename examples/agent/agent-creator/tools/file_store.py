"""FileStore — shared in-memory file tree state for agent-creator tools.

Maintains a module-level ``files`` dict mapping ``path`` → ``content``,
plus a ``finalized`` flag.  Both the LLM loop and the importable tools
share this module, so an imported tool's ``execute()`` reads/writes the
same state that the loop's ``tool_map[name].execute()`` accesses.
"""

# path → content (full file content, never truncated)
files: dict[str, str] = {}
finalized: bool = False
