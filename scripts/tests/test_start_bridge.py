from pathlib import Path


SCRIPT = Path(__file__).parents[1] / "start_bridge.sh"


def test_development_reload_is_scoped_to_bridge_sources():
    """Development reload must not watch mutable Agent package workspaces."""
    content = SCRIPT.read_text(encoding="utf-8")

    assert '--reload-dir "$AR_PROJECT_ROOT/frontend/bridge"' in content


def test_production_mode_does_not_enable_reload():
    """Production Bridge runs without a file reload supervisor."""
    content = SCRIPT.read_text(encoding="utf-8")

    production_block = content.rsplit("production)", 1)[1].split(";;", 1)[0]
    assert "RELOAD=()" in production_block
    assert "--reload" not in production_block
    assert 'AF_BR_ENV="prod"' in production_block


def test_restart_finds_the_real_port_owner_without_lsof():
    content = SCRIPT.read_text(encoding="utf-8")

    assert 'fuser -n tcp "$PORT"' in content
    assert '"/dev/tcp/127.0.0.1/$port"' in content
    assert 'lsof -i :"$port"' not in content
    assert '_is_bridge_pid "$candidate"' in content
    assert 'AR_PROJECT_ROOT/frontend/bridge' in content
