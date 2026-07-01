#!/usr/bin/env python3
"""
Direct TCP JSON-RPC test to the agentflowd daemon.

Tests the basic agent.execute → agent.response notification flow
without going through the bridge WebSocket layer.

Usage:
    python3 frontend/bridge/tests/tcp_direct_test.py
"""

import asyncio
import json
import logging
import sys

HOST = "127.0.0.1"
PORT = 17891
TIMEOUT = 30

logging.basicConfig(level=logging.DEBUG, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("tcp-test")


class DaemonTester:
    """Simple raw-TCP JSON-RPC 2.0 client for testing."""

    def __init__(self, host: str = HOST, port: int = PORT):
        self._host = host
        self._port = port
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._id = 0

    async def connect(self):
        self._reader, self._writer = await asyncio.open_connection(self._host, self._port)
        log.info("Connected to %s:%s", self._host, self._port)

    async def close(self):
        if self._writer:
            self._writer.close()
            try:
                await self._writer.wait_closed()
            except Exception:
                pass
        log.info("Connection closed")

    async def send_request(self, method: str, params: dict | None = None,
                           timeout: float = TIMEOUT) -> dict:
        """Send a JSON-RPC 2.0 request and wait for the response."""
        self._id += 1
        req = {
            "jsonrpc": "2.0",
            "method": method,
            "params": params or {},
            "id": self._id,
        }
        payload = json.dumps(req) + "\n"
        self._writer.write(payload.encode())
        await self._writer.drain()
        log.debug("-> %s (id=%d)", method, self._id)

        # Read until we get a response with matching id
        buf = b""
        while True:
            line = await asyncio.wait_for(self._reader.readline(), timeout=timeout)
            if not line:
                raise ConnectionError("Connection closed")
            text = line.decode().strip()
            if not text:
                continue
            try:
                msg = json.loads(text)
            except json.JSONDecodeError:
                continue

            # Check if it's a notification (no id) or a response with matching id
            msg_id = msg.get("id")
            if msg_id is None:
                # This is a notification — log it
                method_name = msg.get("method", "unknown")
                params_body = msg.get("params", {})
                log.info("<< [NOTIFICATION] method=%s params=%s", method_name,
                         json.dumps(params_body, ensure_ascii=False)[:200])
                # If it's an agent.response for agent.execute, return it
                if method_name == "agent.response":
                    return msg
                continue

            if msg_id == self._id:
                log.debug("<- %s (id=%d)", method, self._id)
                return msg

    async def health(self) -> dict:
        return await self.send_request("health")

    async def agent_execute(self, agent_id: str, content: str,
                            user_id: str = "test-user",
                            client_id: str = "tcp-test",
                            dialogue_id: str = "") -> dict:
        params = {
            "agent_id": agent_id,
            "user_id": user_id,
            "client_id": client_id,
            "content": content,
        }
        if dialogue_id:
            params["dialogue_id"] = dialogue_id
        return await self.send_request("agent.execute", params)

    async def agent_poll_response(self, agent_id: str, session_id: str = "",
                                  task_id: str = "", timeout_ms: int = 5000) -> dict:
        params = {
            "agent_id": agent_id,
            "session_id": session_id,
            "task_id": task_id,
            "timeout_ms": timeout_ms,
        }
        return await self.send_request("agent.poll_response", params)


async def main():
    t = DaemonTester()
    await t.connect()

    # 1. Health check
    health_resp = await t.health()
    result = health_resp.get("result", {})
    log.info("Health: status=%s", result.get("status"))
    assert result.get("status") == "ok", "Health check failed"

    # 2. agent.execute with user input
    log.info("\n--- agent.execute ---")
    execute_resp = await t.agent_execute(
        agent_id="harness-agent",
        content="你好，我的名字是张三",
        dialogue_id="test-dialogue-001",
    )
    execute_result = execute_resp.get("result", {})
    message_id = execute_result.get("message_id", "")
    log.info("agent.execute result: message_id=%s status=%s",
             message_id, execute_result.get("status"))

    assert message_id, f"Missing message_id, result={execute_result}"
    log.info("✅ agent.execute succeeded, message_id=%s", message_id)

    # 3. Wait for agent.response notification
    # The daemon's ResponsePoller will push the response as a notification
    log.info("\n--- Waiting for agent.response notification ---")
    try:
        notify_resp = await asyncio.wait_for(
            t.send_request("agent.poll_response", {
                "agent_id": "harness-agent",
                "timeout_ms": 15000,
            }),
            timeout=20,
        )
        log.info("poll_response result: %s", json.dumps(notify_resp, ensure_ascii=False)[:300])
    except asyncio.TimeoutError:
        log.warning("⚠️  agent.poll_response timed out (no response yet)")
        # Keep listening for notifications
        log.info("Trying to read any pending data from socket...")
        try:
            data = await asyncio.wait_for(t._reader.read(4096), timeout=3)
            log.info("Raw data: %s", data.decode()[:500])
        except asyncio.TimeoutError:
            log.info("No data on socket")

    log.info("\n=== Done ===")
    await t.close()


if __name__ == "__main__":
    asyncio.run(main())
