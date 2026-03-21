"""
AgentFlow DataBus Client SDK for Python

Provides real-time DataBus access via Unix Domain Socket with session/dialogue isolation.
Supports chain-call API matching the C++ INodeDataBusClient interface.

Usage:
    from agentflow_databus import DataBusClient

    # Connect via UDS (uses AF_DATABUS_UDS_FILE environment variable)
    client = DataBusClient.from_env()

    # Or explicit path
    client = DataBusClient("/tmp/agentflow_session_xxx.sock")

    # Chain-call style (matches C++ API)
    client.user().put("my_key", "value")
    value = client.user().get("my_key")
    client.sys().put("response", "Hello World")

    # With dialogue_id
    client.user("dialogue-123").put("skill_result", {"status": "ok"})

    # Subscription
    def on_change(key, value):
        print(f"Key {key} changed to {value}")

    sub_id = client.user().subscribe("my_key", on_change)
    client.user().unsubscribe(sub_id)

    # Raw access
    client.raw().put("/global/config", "value")
"""

import json
import os
import socket
import threading
import uuid
from typing import Any, Callable, Dict, Optional, Union


class DataBusError(Exception):
    """Exception raised for DataBus errors."""
    pass


class JSONRPCError(DataBusError):
    """Exception raised for JSON-RPC protocol errors."""
    def __init__(self, code: int, message: str):
        self.code = code
        self.message = message
        super().__init__(f"JSON-RPC Error {code}: {message}")


class NamespaceClient:
    """
    Namespace-scoped DataBus client.

    Provides key-value operations with automatic namespace prefix.
    All keys are automatically prefixed with /session/{session_id}/{namespace}/
    """

    def __init__(self, client: 'DataBusClient', namespace: str, dialogue_id: Optional[str] = None):
        """
        Initialize namespace client.

        Args:
            client: The parent DataBusClient
            namespace: The namespace (sys, ctx, llm, tool, user)
            dialogue_id: Optional dialogue ID for dialogue-scoped access
        """
        self._client = client
        self._namespace = namespace
        self._dialogue_id = dialogue_id
        self._subscriptions: Dict[int, Callable[[str, Any], None]] = {}
        self._lock = threading.Lock()

    def put(self, key: str, value: Any) -> bool:
        """
        Store a value with the given key.

        Args:
            key: The key name
            value: The value to store (will be JSON-encoded)

        Returns:
            True if successful
        """
        params = {
            "session_id": self._client.session_id,
            "namespace": self._namespace,
            "key": key,
            "value": value
        }
        result = self._client._call("put", params)
        return result.get("success", False)

    def get(self, key: str, default: Any = None) -> Any:
        """
        Retrieve a value by key.

        Args:
            key: The key name
            default: Default value if key not found

        Returns:
            The value or default if not found
        """
        params = {
            "session_id": self._client.session_id,
            "namespace": self._namespace,
            "key": key
        }
        result = self._client._call("get", params)
        if result.get("exists", False):
            return result.get("value", default)
        return default

    def remove(self, key: str) -> bool:
        """
        Remove a key-value pair.

        Args:
            key: The key name

        Returns:
            True if successful
        """
        params = {
            "session_id": self._client.session_id,
            "namespace": self._namespace,
            "key": key
        }
        result = self._client._call("remove", params)
        return result.get("success", False)

    def contains(self, key: str) -> bool:
        """
        Check if a key exists.

        Args:
            key: The key name

        Returns:
            True if the key exists
        """
        params = {
            "session_id": self._client.session_id,
            "namespace": self._namespace,
            "key": key
        }
        result = self._client._call("contains", params)
        return result.get("exists", False)

    def subscribe(self, key: str, callback: Callable[[str, Any], None]) -> int:
        """
        Subscribe to data changes for a specific key.

        Args:
            key: The key name
            callback: Function to call when value changes (key, value)

        Returns:
            Subscription ID for unsubscribing
        """
        params = {
            "session_id": self._client.session_id,
            "namespace": self._namespace,
            "key": key
        }
        result = self._client._call("subscribe", params)
        sub_id = result.get("subscription_id", 0)

        with self._lock:
            self._subscriptions[sub_id] = callback

        return sub_id

    def unsubscribe(self, subscription_id: int) -> bool:
        """
        Cancel a subscription.

        Args:
            subscription_id: The subscription ID returned from subscribe()

        Returns:
            True if successful
        """
        params = {
            "subscription_id": subscription_id
        }
        result = self._client._call("unsubscribe", params)

        with self._lock:
            self._subscriptions.pop(subscription_id, None)

        return result.get("success", False)

    def publish(self, topic: str, value: Any) -> bool:
        """
        Publish an event to a topic.

        Args:
            topic: The topic name
            value: The event payload

        Returns:
            True if successful
        """
        params = {
            "session_id": self._client.session_id,
            "namespace": self._namespace,
            "topic": topic,
            "value": value
        }
        result = self._client._call("publish", params)
        return result.get("success", False)


class RawDataBusClient:
    """
    Raw DataBus client without namespace prefix.

    Use with caution - keys are not prefixed.
    Primarily for reading global configuration.
    """

    def __init__(self, client: 'DataBusClient'):
        self._client = client

    def put(self, key: str, value: Any) -> bool:
        """Store a value with the given key (raw, no prefix)."""
        # Use sys namespace but with full key path
        params = {
            "session_id": self._client.session_id,
            "namespace": "",
            "key": key.lstrip("/"),
            "value": value
        }
        # Build the key directly
        result = self._client._call("put", {
            "session_id": "",
            "namespace": "",
            "key": key.lstrip("/"),
            "value": value
        })
        return result.get("success", False)

    def get(self, key: str, default: Any = None) -> Any:
        """Retrieve a value by key (raw, no prefix)."""
        result = self._client._call("get", {
            "session_id": "",
            "namespace": "",
            "key": key.lstrip("/")
        })
        if result.get("exists", False):
            return result.get("value", default)
        return default

    def contains(self, key: str) -> bool:
        """Check if a key exists (raw, no prefix)."""
        result = self._client._call("contains", {
            "session_id": "",
            "namespace": "",
            "key": key.lstrip("/")
        })
        return result.get("exists", False)

    def remove(self, key: str) -> bool:
        """Remove a key-value pair (raw, no prefix)."""
        result = self._client._call("remove", {
            "session_id": "",
            "namespace": "",
            "key": key.lstrip("/")
        })
        return result.get("success", False)


class DataBusClient:
    """
    Main DataBus client for multi-language access via Unix Domain Socket.

    Provides namespace-scoped access with automatic session prefix.
    Chain-call API matches C++ INodeDataBusClient interface.

    Key structure: /session/{session_id}/{namespace}/{key}
    """

    def __init__(self, socket_path: str, session_id: Optional[str] = None):
        """
        Initialize the DataBus client.

        Args:
            socket_path: Path to the Unix Domain Socket
            session_id: Session ID (extracted from socket path if not provided)
        """
        self._socket_path = socket_path
        self._socket: Optional[socket.socket] = None
        self._lock = threading.Lock()
        self._request_id = 0
        self._connected = False

        # Extract session_id from socket path if not provided
        if session_id:
            self._session_id = session_id
        else:
            # Extract from socket path: /tmp/agentflow_{session_id}.sock
            import os
            basename = os.path.basename(socket_path)
            if basename.startswith("agentflow_") and basename.endswith(".sock"):
                self._session_id = basename[10:-5]  # Remove "agentflow_" and ".sock"
            else:
                self._session_id = ""

        # Cached namespace clients
        self._sys_client: Optional[NamespaceClient] = None
        self._ctx_client: Optional[NamespaceClient] = None
        self._llm_client: Optional[NamespaceClient] = None
        self._tool_client: Optional[NamespaceClient] = None
        self._user_client: Optional[NamespaceClient] = None
        self._raw_client: Optional[RawDataBusClient] = None

        # Connect immediately
        self._connect()

    @classmethod
    def from_env(cls) -> 'DataBusClient':
        """
        Create a client using the AF_DATABUS_UDS_FILE environment variable.

        Returns:
            DataBusClient instance

        Raises:
            DataBusError: If AF_DATABUS_UDS_FILE is not set
        """
        socket_path = os.environ.get("AF_DATABUS_UDS_FILE")
        if not socket_path:
            raise DataBusError("AF_DATABUS_UDS_FILE environment variable not set")
        return cls(socket_path)

    @property
    def session_id(self) -> str:
        """Get the session ID."""
        return self._session_id

    @property
    def socket_path(self) -> str:
        """Get the socket path."""
        return self._socket_path

    def _connect(self):
        """Connect to the Unix Domain Socket."""
        with self._lock:
            if self._connected:
                return

            self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                self._socket.connect(self._socket_path)
                self._connected = True
            except Exception as e:
                self._socket.close()
                self._socket = None
                raise DataBusError(f"Failed to connect to {self._socket_path}: {e}")

    def close(self):
        """Close the connection."""
        with self._lock:
            if self._socket:
                self._socket.close()
                self._socket = None
            self._connected = False

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

    def _call(self, method: str, params: Dict[str, Any]) -> Dict[str, Any]:
        """
        Make a JSON-RPC call.

        Args:
            method: The method name
            params: The parameters

        Returns:
            The result dict

        Raises:
            JSONRPCError: If the server returns an error
            DataBusError: If communication fails
        """
        with self._lock:
            self._request_id += 1
            request_id = self._request_id

            request = {
                "jsonrpc": "2.0",
                "method": method,
                "params": params,
                "id": request_id
            }

            try:
                # Send request
                message = json.dumps(request) + "\n"
                self._socket.sendall(message.encode('utf-8'))

                # Receive response
                response_data = b""
                while True:
                    chunk = self._socket.recv(4096)
                    if not chunk:
                        raise DataBusError("Connection closed")
                    response_data += chunk
                    if b"\n" in response_data:
                        break

                response_str = response_data.decode('utf-8').strip()
                response = json.loads(response_str)

                # Check for error
                if "error" in response:
                    error = response["error"]
                    raise JSONRPCError(
                        error.get("code", -1),
                        error.get("message", "Unknown error")
                    )

                return response.get("result", {})

            except socket.error as e:
                self._connected = False
                raise DataBusError(f"Socket error: {e}")
            except json.JSONDecodeError as e:
                raise DataBusError(f"Invalid JSON response: {e}")

    def sys(self, dialogue_id: Optional[str] = None) -> NamespaceClient:
        """
        Access sys namespace (framework communication).

        Args:
            dialogue_id: Optional dialogue ID for dialogue-scoped access

        Returns:
            NamespaceClient for sys namespace
        """
        if dialogue_id:
            return NamespaceClient(self, "sys", dialogue_id)
        if not self._sys_client:
            self._sys_client = NamespaceClient(self, "sys")
        return self._sys_client

    def ctx(self, dialogue_id: Optional[str] = None) -> NamespaceClient:
        """
        Access ctx namespace (context storage).

        Args:
            dialogue_id: Optional dialogue ID for dialogue-scoped access

        Returns:
            NamespaceClient for ctx namespace
        """
        if dialogue_id:
            return NamespaceClient(self, "ctx", dialogue_id)
        if not self._ctx_client:
            self._ctx_client = NamespaceClient(self, "ctx")
        return self._ctx_client

    def llm(self, dialogue_id: Optional[str] = None) -> NamespaceClient:
        """
        Access llm namespace (LLM service interaction).

        Args:
            dialogue_id: Optional dialogue ID for dialogue-scoped access

        Returns:
            NamespaceClient for llm namespace
        """
        if dialogue_id:
            return NamespaceClient(self, "llm", dialogue_id)
        if not self._llm_client:
            self._llm_client = NamespaceClient(self, "llm")
        return self._llm_client

    def tool(self, dialogue_id: Optional[str] = None) -> NamespaceClient:
        """
        Access tool namespace (tool call information).

        Args:
            dialogue_id: Optional dialogue ID for dialogue-scoped access

        Returns:
            NamespaceClient for tool namespace
        """
        if dialogue_id:
            return NamespaceClient(self, "tool", dialogue_id)
        if not self._tool_client:
            self._tool_client = NamespaceClient(self, "tool")
        return self._tool_client

    def user(self, dialogue_id: Optional[str] = None) -> NamespaceClient:
        """
        Access user namespace (user-defined data).

        Args:
            dialogue_id: Optional dialogue ID for dialogue-scoped access

        Returns:
            NamespaceClient for user namespace
        """
        if dialogue_id:
            return NamespaceClient(self, "user", dialogue_id)
        if not self._user_client:
            self._user_client = NamespaceClient(self, "user")
        return self._user_client

    def raw(self) -> RawDataBusClient:
        """
        Get raw DataBus access (bypasses session isolation).

        Use with caution - keys are not prefixed.
        Primarily for reading global configuration.

        Returns:
            RawDataBusClient instance
        """
        if not self._raw_client:
            self._raw_client = RawDataBusClient(self)
        return self._raw_client


# Convenience function for quick access
def get_client() -> DataBusClient:
    """
    Get a DataBus client using environment variables.

    Returns:
        DataBusClient instance

    Raises:
        DataBusError: If AF_DATABUS_UDS_FILE is not set
    """
    return DataBusClient.from_env()