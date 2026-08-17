"""Assert that split functions live in their designated modules.

Structural refactor (2026-08-14): skill_executor.py was split into
artifact_pipeline, tool_registry, and skills_loader.  This test verifies
that each moved function's __module__ points to the new owning module,
proving the split is real (not just a re-import wrapper).
"""

import inspect


def test_extract_artifact_paths_defined_in_artifact_pipeline():
    from agentflow_runtime.skill_executor import extract_artifact_paths
    mod = inspect.getmodule(extract_artifact_paths)
    assert mod is not None
    assert mod.__name__ == "agentflow_runtime.artifact_pipeline", (
        f"extract_artifact_paths should be defined in artifact_pipeline, "
        f"got {mod.__name__}"
    )


def test_extract_structured_artifacts_defined_in_artifact_pipeline():
    from agentflow_runtime.skill_executor import extract_structured_artifacts
    mod = inspect.getmodule(extract_structured_artifacts)
    assert mod is not None
    assert mod.__name__ == "agentflow_runtime.artifact_pipeline", (
        f"extract_structured_artifacts should be defined in artifact_pipeline, "
        f"got {mod.__name__}"
    )


def test_extract_all_artifacts_defined_in_artifact_pipeline():
    from agentflow_runtime.skill_executor import extract_all_artifacts
    mod = inspect.getmodule(extract_all_artifacts)
    assert mod is not None
    assert mod.__name__ == "agentflow_runtime.artifact_pipeline", (
        f"extract_all_artifacts should be defined in artifact_pipeline, "
        f"got {mod.__name__}"
    )


def test_ensure_artifacts_in_package_defined_in_artifact_pipeline():
    from agentflow_runtime.skill_executor import _ensure_artifacts_in_package
    mod = inspect.getmodule(_ensure_artifacts_in_package)
    assert mod is not None
    assert mod.__name__ == "agentflow_runtime.artifact_pipeline", (
        f"_ensure_artifacts_in_package should be defined in artifact_pipeline, "
        f"got {mod.__name__}"
    )


def test_LocalToolRegistry_defined_in_tool_registry():
    from agentflow_runtime.skill_executor import _LocalToolRegistry
    mod = inspect.getmodule(_LocalToolRegistry)
    assert mod is not None
    assert mod.__name__ == "agentflow_runtime.tool_registry", (
        f"_LocalToolRegistry should be defined in tool_registry, "
        f"got {mod.__name__}"
    )


def test_parse_agent_md_defined_in_skills_loader():
    from agentflow_runtime.skill_executor import parse_agent_md
    mod = inspect.getmodule(parse_agent_md)
    assert mod is not None
    assert mod.__name__ == "agentflow_runtime.skills_loader", (
        f"parse_agent_md should be defined in skills_loader, "
        f"got {mod.__name__}"
    )


def test_load_skill_md_defined_in_skills_loader():
    from agentflow_runtime.skill_executor import load_skill_md
    mod = inspect.getmodule(load_skill_md)
    assert mod is not None
    assert mod.__name__ == "agentflow_runtime.skills_loader", (
        f"load_skill_md should be defined in skills_loader, "
        f"got {mod.__name__}"
    )


def test_load_skills_from_list_defined_in_skills_loader():
    from agentflow_runtime.skill_executor import load_skills_from_list
    mod = inspect.getmodule(load_skills_from_list)
    assert mod is not None
    assert mod.__name__ == "agentflow_runtime.skills_loader", (
        f"load_skills_from_list should be defined in skills_loader, "
        f"got {mod.__name__}"
    )


def test_expand_env_defined_in_tool_registry():
    from agentflow_runtime.skill_executor import _expand_env
    mod = inspect.getmodule(_expand_env)
    assert mod is not None
    assert mod.__name__ == "agentflow_runtime.tool_registry", (
        f"_expand_env should be defined in tool_registry, "
        f"got {mod.__name__}"
    )


def test_load_dir_defined_in_tool_registry():
    from agentflow_runtime.skill_executor import _load_dir
    mod = inspect.getmodule(_load_dir)
    assert mod is not None
    assert mod.__name__ == "agentflow_runtime.tool_registry", (
        f"_load_dir should be defined in tool_registry, "
        f"got {mod.__name__}"
    )


def test_functions_retained_in_skill_executor():
    """Functions that should STAY in skill_executor (not moved)."""
    from agentflow_runtime.skill_executor import (
        execute,
        _detect_tool_call_loop,
        _summarize_tool_history,
        validate_tool_calls,
        remove_invalid_tool_turns,
        _inject_venv_env,
        _load_dotenv,
        _get_agent_package_dir,
        _get_skills_base,
        _get_builtin_tools_dir,
        _get_agent_tools_dir,
    )

    # These should be defined IN skill_executor (their __module__ should be
    # agentflow_runtime.skill_executor)
    for func in [execute, _detect_tool_call_loop, _summarize_tool_history,
                 validate_tool_calls, remove_invalid_tool_turns,
                 _inject_venv_env, _load_dotenv,
                 _get_agent_package_dir, _get_skills_base,
                 _get_builtin_tools_dir, _get_agent_tools_dir]:
        mod = inspect.getmodule(func)
        assert mod is not None
        assert mod.__name__ == "agentflow_runtime.skill_executor", (
            f"{func.__name__} should stay in skill_executor, "
            f"got {mod.__name__}"
        )
