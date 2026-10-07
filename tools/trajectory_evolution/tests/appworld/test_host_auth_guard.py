"""Narrow host feedback for authenticated Spotify reads missing their token."""

from __future__ import annotations

import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[4]
ADAPTER = ROOT / "examples/agent/business-simulator/appworld"
sys.path.insert(0, str(ADAPTER))

from appworld_host import appworld_missing_access_token  # noqa: E402


@pytest.mark.parametrize("method", ["show_account", "show_album_library"])
def test_protected_spotify_read_missing_token_is_rejected_after_login(method):
    """Prevent the observed 401/re-login detour using only public API facts."""
    docs = json.loads((ADAPTER / "runtime/world/data/api_docs/standard/spotify.json").read_text())
    assert any(parameter["name"] == "access_token" and parameter["required"]
               for parameter in docs[method]["parameters"])
    assert appworld_missing_access_token(f"result = apis.spotify.{method}()", {"spotify"})
    assert not appworld_missing_access_token(
        f"result = apis.spotify.{method}(access_token=spotify_token)", {"spotify"})


@pytest.mark.parametrize("authenticated_apps", [set(), {"venmo"}])
@pytest.mark.parametrize("method", ["show_account", "show_album_library"])
def test_spotify_guard_requires_successful_spotify_login(authenticated_apps, method):
    assert not appworld_missing_access_token(f"apis.spotify.{method}()", authenticated_apps)


@pytest.mark.parametrize("code", [
    "apis.spotify.search_songs(query='fixture')",
    "apis.spotify.show_song(song_id=1)",
    "apis.spotify.show_album(album_id=1)",
    "apis.api_docs.show_api_doc(app_name='spotify', api_name='show_account')",
])
def test_spotify_guard_does_not_expand_to_public_catalog_or_docs_reads(code):
    assert not appworld_missing_access_token(code, {"spotify"})
