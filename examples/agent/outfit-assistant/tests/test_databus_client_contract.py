"""Contract tests for databus_client.py — wrapper-to-SDK API mapping.

Verifies that the ``DataBusClient`` wrapper's method calls correctly map to the
underlying ``agentflow_databus`` SDK's real API surface.  These tests mock the
SDK at the ``sys()`` / ``ctx()`` level and assert the right method name is
invoked — they do NOT test UDS transport behaviour.

Why this matters (F-20260704):
  - The wrapper's ``delete()`` was calling ``sys().delete()`` but the SDK's
    ``NamespaceClient`` exposes ``remove()``, not ``delete()``.
  - Unit tests with permissive fakes (``FakeDataBus.delete()``) did not catch
    the mismatch because the fake accepted any method name.
  - Only a contract test that inspects the *actual SDK method name* can prevent
    this class of defect.
"""

import os
import sys
from pathlib import Path
from unittest.mock import MagicMock, PropertyMock, call

import pytest

# ── Import the real module (uses sys.path injection like the nodes do) ──
_NODES_DIR = Path(__file__).parents[1] / "nodes"
sys.path.insert(0, str(_NODES_DIR))

# Mock the SDK *before* importing the wrapper so the real SDK is never
# instantiated (no UDS socket needed).
_sdk_module = type(sys)("agentflow_databus")
_sdk_module.DataBusClient = MagicMock  # will be replaced per-test
_sdk_module.DataBusError = type("DataBusError", (Exception,), {})
sys.modules["agentflow_databus"] = _sdk_module

from databus_client import DataBusClient


# ── Fixtures ──


@pytest.fixture
def mock_sdk_client() -> MagicMock:
    """Return a fresh ``MagicMock`` standing in for the SDK ``DataBusClient``.

    The mock's ``sys()`` and ``ctx()`` methods return ``NamespaceClient``
    mocks that record every call.
    """
    return MagicMock()


@pytest.fixture
def wrapper(mock_sdk_client: MagicMock, monkeypatch) -> DataBusClient:
    """Return a ``DataBusClient`` wrapper whose ``_client`` is the mock SDK client."""
    monkeypatch.setenv("AF_DATABUS_UDS_FILE", "/tmp/test-databus.sock")
    monkeypatch.setenv("AF_SESSION_ID", "test-session")
    monkeypatch.setenv("AF_DIALOGUE_ID", "test-dialogue")
    wrapper = DataBusClient()
    wrapper._client = mock_sdk_client
    wrapper._dialogue_id = "test-dialogue"
    return wrapper


# ═══════════════════════════════════════════════════════════════════
# Contract: delete → NamespaceClient.remove
# ═══════════════════════════════════════════════════════════════════


class TestDeleteContract:
    """Verify that ``DataBusClient.delete()`` maps to the SDK's ``remove()``."""

    def test_delete_calls_sys_remove(self, wrapper, mock_sdk_client):
        """delete('my_key') must call sys().remove('test-dialogue/my_key')."""
        # Arrange
        ns_mock = MagicMock()
        mock_sdk_client.sys.return_value = ns_mock

        # Act
        wrapper.delete("my_key")

        # Assert: the SDK's sys().remove() was called (NOT sys().delete())
        mock_sdk_client.sys.assert_called_once()
        ns_mock.remove.assert_called_once_with("test-dialogue/my_key")

    def test_delete_does_not_call_sys_delete(self, wrapper, mock_sdk_client):
        """delete() must NOT call sys().delete() — that method does not exist on the SDK."""
        # Arrange
        ns_mock = MagicMock(spec=[])  # no methods — accessing .delete would raise
        mock_sdk_client.sys.return_value = ns_mock

        # Act
        with pytest.raises(AttributeError):
            # Calling .delete on a spec-limited mock proves the wrapper
            # would also fail if it used .delete instead of .remove.
            ns_mock.delete("x")

    def test_delete_no_dialogue_id(self, wrapper, mock_sdk_client):
        """Without dialogue_id, delete('key') calls sys().remove('key') (no prefix)."""
        # Arrange
        wrapper._dialogue_id = ""
        ns_mock = MagicMock()
        mock_sdk_client.sys.return_value = ns_mock

        # Act
        wrapper.delete("bare_key")

        # Assert
        ns_mock.remove.assert_called_once_with("bare_key")

    def test_delete_returns_true_on_success(self, wrapper, mock_sdk_client):
        """delete returns True when the SDK call succeeds."""
        # Arrange
        ns_mock = MagicMock()
        mock_sdk_client.sys.return_value = ns_mock

        # Act
        result = wrapper.delete("k")

        # Assert
        assert result is True

    def test_delete_returns_false_on_sdk_exception(self, wrapper, mock_sdk_client):
        """delete returns False (not crashes) when the SDK raises."""
        # Arrange
        ns_mock = MagicMock()
        ns_mock.remove.side_effect = RuntimeError("UDS write error")
        mock_sdk_client.sys.return_value = ns_mock

        # Act
        result = wrapper.delete("k")

        # Assert: silently handled, no exception propagates
        assert result is False


# ═══════════════════════════════════════════════════════════════════
# Contract: put / get (sanity baseline)
# ═══════════════════════════════════════════════════════════════════


class TestPutGetContract:
    """Quick sanity that other wrapper methods also map correctly."""

    def test_put_calls_sys_put(self, wrapper, mock_sdk_client):
        """put('k', 'v') must call sys().put('dialogue/k', 'v')."""
        ns_mock = MagicMock()
        mock_sdk_client.sys.return_value = ns_mock

        wrapper.put("color", "red")

        ns_mock.put.assert_called_once_with("test-dialogue/color", "red")

    def test_get_calls_sys_get(self, wrapper, mock_sdk_client):
        """get('k') must call sys().get('dialogue/k', None)."""
        ns_mock = MagicMock()
        mock_sdk_client.sys.return_value = ns_mock

        wrapper.get("color")

        ns_mock.get.assert_called_once_with("test-dialogue/color", None)

    def test_put_global_calls_sys_put_no_prefix(self, wrapper, mock_sdk_client):
        """put_global('k', 'v') must call sys().put('k', 'v') — no dialogue prefix."""
        ns_mock = MagicMock()
        mock_sdk_client.sys.return_value = ns_mock

        wrapper.put_global("_e2e_response", "done")

        ns_mock.put.assert_called_once_with("_e2e_response", "done")


# ═══════════════════════════════════════════════════════════════════
# Contract: response_push
# ═══════════════════════════════════════════════════════════════════


class TestResponsePushContract:
    """Verify that ``DataBusClient.response_push()`` delegates to the SDK's ``response_push()``."""

    def test_response_push_delegates_with_source(self, wrapper, mock_sdk_client):
        """response_push(content, type=..., source=...) must call SDK's response_push with source."""
        wrapper.response_push("test content", type="progress", source="coordinator")
        mock_sdk_client.response_push.assert_called_once_with(
            "test content", type="progress", source="coordinator",
            dialogue_id="test-dialogue",
        )

    def test_response_push_delegates_with_all_params(self, wrapper, mock_sdk_client):
        """response_push with all optional params delegates correctly."""
        wrapper.response_push(
            "llm chunk", type="llm_chunk", source="skill_executor",
            skill="weather_query", round="1", status="running",
        )
        mock_sdk_client.response_push.assert_called_once_with(
            "llm chunk", type="llm_chunk", source="skill_executor",
            skill="weather_query", round="1", status="running",
            dialogue_id="test-dialogue",
        )

    def test_response_push_without_source(self, wrapper, mock_sdk_client):
        """response_push without source still delegates to SDK (source defaults to empty string)."""
        wrapper.response_push("content", type="progress")
        mock_sdk_client.response_push.assert_called_once_with(
            "content", type="progress",
            dialogue_id="test-dialogue",
        )

    def test_response_push_returns_sdk_result(self, wrapper, mock_sdk_client):
        """response_push returns the SDK's return value directly."""
        mock_sdk_client.response_push.return_value = True
        result = wrapper.response_push("ok", type="text")
        assert result is True

    def test_response_push_not_connected(self, wrapper, mock_sdk_client):
        """When _client is None, response_push returns False without crashing."""
        wrapper._client = None
        result = wrapper.response_push("x", type="progress")
        assert result is False
