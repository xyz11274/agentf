#!/usr/bin/env python3
"""
AgentFlow Daemon Test Client — sends a request via JSON-RPC and polls for response.

Usage:
  # Start agentflowd first (in another terminal)
  ./build/agentflowd --package "$(pwd)/examples/agent/harness_agent_v2"

  # Then run this client
  python3 examples/agent/harness_agent_v2/test_client.py

  # With custom message
  python3 examples/agent/harness_agent_v2/test_client.py --message "请用 echo skill 测试一下链路"
"""

import argparse
import json
import socket
import sys
import time


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 7890
TIMEOUT_MS = 60000


def json_rpc_request(method: str, params: dict, req_id: int = 1) -> str:
    return json.dumps({
        "jsonrpc": "2.0",
        "method": method,
        "params": params,
        "id": req_id
    })


def send_and_recv(sock: socket.socket, data: str) -> str:
    sock.sendall((data + "\n").encode("utf-8"))
    buf = b""
    while True:
        ch = sock.recv(1)
        if not ch or ch == b"\n":
            break
        buf += ch
    return buf.decode("utf-8")


def main():
    parser = argparse.ArgumentParser(description="AgentFlow V2 test client")
    parser.add_argument("--host", default=DEFAULT_HOST, help="Daemon host")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="Daemon port")
    parser.add_argument("--agent-id", default="harness-agent-v2", help="Agent ID to test")
    parser.add_argument("--message", default="你好！请用 echo skill 测试一下链路是否通畅", help="Message to send")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.connect((args.host, args.port))
    except ConnectionRefusedError:
        print(f"[ERROR] Cannot connect to {args.host}:{args.port}")
        print("Make sure agentflowd is running:")
        print(f"  ./build/agentflowd --package \"$(pwd)/examples/agent/harness_agent_v2\"")
        sys.exit(1)

    # === Step 1: Health check ===
    print(f"[1] Health check...")
    resp = send_and_recv(sock, json_rpc_request("health", {}))
    data = json.loads(resp)
    assert data.get("result", {}).get("status") == "ok", f"Health check failed: {data}"
    print(f"    OK — version={data['result'].get('version')}")

    # === Step 2: List agents ===
    print(f"[2] List agents...")
    resp = send_and_recv(sock, json_rpc_request("agent.list", {}))
    data = json.loads(resp)
    agents = data.get("result", {}).get("agents", [])
    agent_ids = [a["agent_id"] for a in agents]
    print(f"    Agents: {agent_ids}")
    assert args.agent_id in agent_ids, f"Agent {args.agent_id} not found in {agent_ids}"

    # === Step 3: Get agent info ===
    print(f"[3] Agent info: {args.agent_id}")
    resp = send_and_recv(sock, json_rpc_request("agent.info", {"agent_id": args.agent_id}))
    data = json.loads(resp)
    workflows = data.get("result", {}).get("workflows", [])
    print(f"    Workflows: {[w['id'] for w in workflows]}")

    # === Step 4: Send a request ===
    print(f"[4] Sending message: \"{args.message}\"")
    resp = send_and_recv(sock, json_rpc_request("agent.execute", {
        "agent_id": args.agent_id,
        "user_id": "test_user",
        "client_id": "test_client",
        "content": args.message
    }))
    data = json.loads(resp)
    message_id = data.get("result", {}).get("message_id", "unknown")
    print(f"    Submitted — message_id={message_id}")

    # === Step 5: Poll for response ===
    print(f"[5] Polling for response (timeout={TIMEOUT_MS}ms)...")
    start = time.time()
    while time.time() - start < TIMEOUT_MS / 1000:
        resp = send_and_recv(sock, json_rpc_request("agent.poll_response", {
            "agent_id": args.agent_id,
            "timeout_ms": TIMEOUT_MS
        }))
        data = json.loads(resp)
        messages = data.get("result", {}).get("messages", [])
        complete = data.get("result", {}).get("complete", False)
        elapsed = int((time.time() - start) * 1000)

        if messages:
            for msg in messages:
                content = msg.get("content", "")
                final = msg.get("final", False)
                if content:
                    print(f"    [{elapsed}ms] Response: {content}")
                if final:
                    print(f"    [{elapsed}ms] (final)")
            if complete:
                break
        else:
            print(f"    [{elapsed}ms] waiting...")
            time.sleep(1)
    else:
        print(f"    [TIMEOUT] No response within {TIMEOUT_MS}ms")

    # === Step 6: Get runtime status ===
    print(f"[6] Runtime status...")
    resp = send_and_recv(sock, json_rpc_request("runtime.status", {}))
    data = json.loads(resp)
    rs = data.get("result", {})
    print(f"    running={rs.get('running')}, agents={rs.get('active_agents')}, "
          f"workflows={rs.get('active_workflows')}, nodes={rs.get('running_nodes')}")

    sock.close()
    print(f"\n[DONE]")


if __name__ == "__main__":
    main()
