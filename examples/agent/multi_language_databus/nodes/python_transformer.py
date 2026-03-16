#!/usr/bin/env python3
"""
Python Data Transformer Node
Reads data from C++ producer, transforms it, and writes back to DataBus
Uses the AgentFlow DataBus Client SDK via UDS
"""

import os
import sys
import json
import time

# Add the clients directory to path for importing the SDK
script_dir = os.path.dirname(os.path.abspath(__file__))
clients_dir = os.path.join(os.path.dirname(os.path.dirname(script_dir)), "..", "..", "clients", "python")
sys.path.insert(0, clients_dir)

try:
    from agentflow_databus import DataBusClient
except ImportError:
    # Fallback: implement minimal UDS client inline
    import socket

    class MinimalDataBusClient:
        def __init__(self):
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.connect(os.environ["AF_DATABUS_UDS_FILE"])
            self.req_id = 0
            self._session_id = ""

        def _extract_session_id(self):
            path = os.environ.get("AF_DATABUS_UDS_FILE", "")
            basename = os.path.basename(path)
            if basename.startswith("agentflow_") and basename.endswith(".sock"):
                return basename[10:-5]
            return ""

        @property
        def session_id(self):
            if not self._session_id:
                self._session_id = self._extract_session_id()
            return self._session_id

        def _call(self, method, params):
            self.req_id += 1
            request = {
                "jsonrpc": "2.0",
                "method": method,
                "params": params,
                "id": self.req_id
            }
            self.sock.sendall((json.dumps(request) + "\n").encode())
            response = json.loads(self.sock.recv(4096).decode().strip())
            if "error" in response:
                raise Exception(response["error"]["message"])
            return response.get("result", {})

        def user(self, dialogue_id=None):
            return MinimalNamespaceClient(self, "user", dialogue_id)

        def sys(self, dialogue_id=None):
            return MinimalNamespaceClient(self, "sys", dialogue_id)

        def close(self):
            self.sock.close()

    class MinimalNamespaceClient:
        def __init__(self, client, namespace, dialogue_id=None):
            self._client = client
            self._namespace = namespace
            self._dialogue_id = dialogue_id

        def _build_key(self, key):
            if self._dialogue_id:
                return f"{self._dialogue_id}/{key}"
            return key

        def put(self, key, value):
            result = self._client._call("put", {
                "session_id": self._client.session_id,
                "namespace": self._namespace,
                "key": self._build_key(key),
                "value": value
            })
            return result.get("success", False)

        def get(self, key, default=None):
            result = self._client._call("get", {
                "session_id": self._client.session_id,
                "namespace": self._namespace,
                "key": self._build_key(key)
            })
            if result.get("exists", False):
                return result.get("value", default)
            return default

    DataBusClient = MinimalDataBusClient


def get_input(name, default=""):
    """Get input from environment variable (AF_* convention)"""
    return os.environ.get(f"AF_{name.upper()}", default)


def main():
    session_id = get_input("session_id", "")
    dialogue_id = get_input("dialogue_id", "")

    # Connect to DataBus via UDS
    client = DataBusClient.from_env() if hasattr(DataBusClient, 'from_env') else DataBusClient()

    try:
        # Build the chain data key
        chain_key = f"{dialogue_id}/chain_data" if dialogue_id else "chain_data"

        # Read data produced by C++ node
        cpp_data = client.user().get(chain_key, "")
        cpp_step = client.user().get(chain_key + "_step", "")

        if not cpp_data:
            print(f"ERROR: No data found at {chain_key}", file=sys.stderr)
            print(json.dumps({"status": "error", "message": "No data from C++ producer"}))
            return 1

        print(f"Python: Read from C++: '{cpp_data}' (step: {cpp_step})", file=sys.stderr)

        # Transform the data
        transformed = f"PYTHON_TRANSFORMED({cpp_data})"

        # Write transformed data back
        client.user().put(chain_key, transformed)
        client.user().put(chain_key + "_step", "python")
        client.user().put(chain_key + "_python_timestamp", str(int(time.time() * 1000)))

        print(f"Python: Wrote transformed data: '{transformed}'", file=sys.stderr)

        # Output result as JSON
        result = {
            "status": "transformed",
            "transformed": transformed,
            "original": cpp_data,
            "session_id": session_id,
            "dialogue_id": dialogue_id
        }
        print(json.dumps(result))

        return 0

    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        print(json.dumps({"status": "error", "message": str(e)}))
        return 1
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())