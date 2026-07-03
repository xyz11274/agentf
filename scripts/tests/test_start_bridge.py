from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "start_bridge.sh"


def test_development_reload_is_scoped_to_bridge_sources():
    """Development reload must not watch mutable Agent package workspaces."""
    content = SCRIPT.read_text(encoding="utf-8")

    assert '--reload-dir "$AR_PROJECT_ROOT/frontend/bridge"' in content


def test_production_mode_does_not_enable_reload():
    """Production Bridge runs without a file reload supervisor."""
    content = SCRIPT.read_text(encoding="utf-8")

    assert 'production) HOST="127.0.0.1"; RELOAD=()' in content
