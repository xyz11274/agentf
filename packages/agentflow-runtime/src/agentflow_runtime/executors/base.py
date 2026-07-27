"""ToolExecutor abstract interface — unified provider pattern for tools runtime.

ADR: 20260724-tools-runtime-refactor.md

This module defines:
- ExecResult / FileContent / WriteResult — uniform result dataclasses
- ToolExecutor — ABC with 6 abstract methods mirroring C++ Sandbox abstraction

Each provider (HostExecutor, HTTPExecutor, E2BExecutor) implements this
interface.  Pure Python, runs on host, loaded by skill_executor.
"""

from abc import ABC, abstractmethod
from dataclasses import asdict, dataclass


# ---------------------------------------------------------------------------
# Result dataclasses
# ---------------------------------------------------------------------------

@dataclass
class ExecResult:
    """Unified command execution result (all providers return this type)."""

    stdout: str = ""
    stderr: str = ""
    exit_code: int = 0
    error: str | None = None
    truncated: bool = False

    def to_dict(self) -> dict:
        """Return a JSON-serializable dict representation."""
        return asdict(self)


@dataclass
class FileContent:
    """Unified file read result."""

    content: str = ""
    error: str | None = None
    total_lines: int = 0

    def to_dict(self) -> dict:
        """Return a JSON-serializable dict representation."""
        return asdict(self)


@dataclass
class WriteResult:
    """Unified file write result."""

    success: bool = False
    error: str | None = None
    bytes_written: int = 0

    def to_dict(self) -> dict:
        """Return a JSON-serializable dict representation."""
        return asdict(self)


# ---------------------------------------------------------------------------
# ToolExecutor ABC
# ---------------------------------------------------------------------------

class ToolExecutor(ABC):
    """Tool execution routing abstraction — parallel to C++ Sandbox interface.

    Each provider implements this interface, encapsulating "how tool operations
    reach the correct execution environment".  Pure Python, does not enter the
    sandbox, loaded by skill_executor.
    """

    @abstractmethod
    def exec_command(self, command: str, timeout: int = 30, workdir: str | None = None) -> ExecResult:
        """Execute a shell command."""
        ...

    @abstractmethod
    def read_file(self, path: str, offset: int = 0, limit: int = 0) -> FileContent:
        """Read file content.  offset/limit of 0 means read entire file.

        offset is 1-based line number to start from.
        limit > 0 limits the number of lines returned.
        """
        ...

    @abstractmethod
    def write_file(self, path: str, content: str) -> WriteResult:
        """Write content to a file (overwrite mode)."""
        ...

    @abstractmethod
    def edit_file(self, path: str, old_str: str, new_str: str, replace_all: bool = False) -> ExecResult:
        """Precisely replace text in a file.

        Without replace_all, old_str must appear exactly once.
        """
        ...

    @abstractmethod
    def list_dir(self, path: str, recursive: bool = False) -> list[dict]:
        """List directory contents.  Returns [{name, path, is_dir}, ...]."""
        ...

    @abstractmethod
    def fetch_url(self, url: str, timeout: int = 30) -> FileContent:
        """Fetch URL content as text."""
        ...
