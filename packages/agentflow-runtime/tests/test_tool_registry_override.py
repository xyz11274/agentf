"""Regression coverage for local Tool implementation/schema overrides."""

from __future__ import annotations

from types import SimpleNamespace

from agentflow_runtime.tool_registry import _load_dir


def _write_tool(directory, filename, *, description, parameter, result):
    directory.mkdir(parents=True, exist_ok=True)
    (directory / filename).write_text(
        (
            f'TOOL_NAME = "shared-tool"\n'
            f'TOOL_DESCRIPTION = "{description}"\n'
            'TOOL_PARAMS = {"type": "object", "properties": '
            f'{{"{parameter}": {{"type": "string"}}}}}}\n'
            'def execute(args, sandbox_endpoint=None):\n'
            f'    return "{result}"\n'
        ),
        encoding="utf-8",
    )


def test_later_tool_override_replaces_schema_and_implementation_in_place(tmp_path):
    builtin_dir = tmp_path / "builtin"
    skill_dir = tmp_path / "skill"
    _write_tool(
        builtin_dir,
        "shared.py",
        description="builtin schema",
        parameter="command",
        result="builtin implementation",
    )
    _write_tool(
        skill_dir,
        "shared.py",
        description="skill schema",
        parameter="script",
        result="skill implementation",
    )
    registry = SimpleNamespace(_modules={}, _tools=[])

    _load_dir(registry, str(builtin_dir))
    _load_dir(registry, str(skill_dir), prefix="_skill_")

    assert len(registry._tools) == 1
    schema = registry._tools[0]["function"]
    assert schema == {
        "name": "shared-tool",
        "description": "skill schema",
        "parameters": {
            "type": "object",
            "properties": {"script": {"type": "string"}},
        },
    }
    assert registry._modules["shared-tool"].execute({}) == "skill implementation"
