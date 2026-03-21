#!/usr/bin/env python3
"""
AgentFlow DataBus Python SDK 基础用法示例

演示通过 UDS 连接 DataBus，进行 put/get/contains/remove/subscribe 操作。

运行前提：AgentFlow Worker 已启动并设置 AF_DATABUS_UDS_FILE 环境变量。
也可以显式指定 socket 路径：
    python basic_usage.py /tmp/agentflow_session_xxx.sock
"""

import os
import sys
import time
import threading

# 将 Python SDK 所在目录加入路径
SDK_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, SDK_DIR)

from agentflow_databus import DataBusClient, DataBusError, JSONRPCError


def demo_basic_operations(client: DataBusClient) -> None:
    """演示基础读写操作。"""
    print("\n=== 基础读写操作 ===")

    # 写入字符串值
    client.user().put("greeting", "Hello, AgentFlow!")
    print("put greeting: Hello, AgentFlow!")

    # 读取值
    value = client.user().get("greeting")
    print(f"get greeting: {value}")

    # 写入复杂对象（自动序列化为 JSON）
    config = {"debug": True, "level": 5, "tags": ["agent", "databus"]}
    client.user().put("config", config)
    print(f"put config: {config}")

    retrieved = client.user().get("config")
    print(f"get config: {retrieved}")

    # 检查键是否存在
    exists = client.user().contains("greeting")
    print(f"contains greeting: {exists}")

    not_exists = client.user().contains("nonexistent_key")
    print(f"contains nonexistent_key: {not_exists}")

    # 删除键
    client.user().remove("greeting")
    print("remove greeting")

    after_remove = client.user().contains("greeting")
    print(f"contains greeting after remove: {after_remove}")


def demo_namespaces(client: DataBusClient) -> None:
    """演示不同命名空间的访问。"""
    print("\n=== 命名空间访问 ===")

    # sys 命名空间：框架通信
    client.sys().put("response", "OK")
    print(f"sys.response = {client.sys().get('response')}")

    # ctx 命名空间：上下文存储
    client.ctx().put("history", ["msg1", "msg2"])
    print(f"ctx.history = {client.ctx().get('history')}")

    # user 命名空间：用户自定义数据
    client.user().put("result", {"status": "completed"})
    print(f"user.result = {client.user().get('result')}")


def demo_dialogue_isolation(client: DataBusClient) -> None:
    """演示多对话数据隔离。"""
    print("\n=== 对话隔离 ===")

    dialogue_a = "dialogue-001"
    dialogue_b = "dialogue-002"

    # 两个对话写入相同键名，数据互不干扰
    client.user(dialogue_a).put("skill_result", "Result from dialogue A")
    client.user(dialogue_b).put("skill_result", "Result from dialogue B")

    result_a = client.user(dialogue_a).get("skill_result")
    result_b = client.user(dialogue_b).get("skill_result")
    print(f"Dialogue A skill_result: {result_a}")
    print(f"Dialogue B skill_result: {result_b}")


def demo_subscription(client: DataBusClient) -> None:
    """演示订阅数据变更。"""
    print("\n=== 订阅变更 ===")

    received = []

    def on_status_change(key: str, value) -> None:
        print(f"  [订阅回调] {key} = {value}")
        received.append(value)

    # 订阅 status 键
    sub_id = client.user().subscribe("status", on_status_change)
    print(f"订阅 status，subscription_id={sub_id}")

    # 写入变更，触发回调
    for state in ["pending", "running", "completed"]:
        client.user().put("status", state)
        time.sleep(0.1)

    # 等待回调处理完毕
    time.sleep(0.2)

    # 取消订阅
    client.user().unsubscribe(sub_id)
    print(f"取消订阅，共收到 {len(received)} 次变更通知")


def main() -> None:
    # 从参数或环境变量获取 socket 路径
    if len(sys.argv) > 1:
        socket_path = sys.argv[1]
        client = DataBusClient(socket_path)
    else:
        client = DataBusClient.from_env()

    print(f"连接到 DataBus: {client.socket_path}")
    print(f"Session ID: {client.session_id}")

    try:
        with client:
            demo_basic_operations(client)
            demo_namespaces(client)
            demo_dialogue_isolation(client)
            demo_subscription(client)
    except DataBusError as e:
        print(f"连接错误: {e}", file=sys.stderr)
        sys.exit(1)
    except JSONRPCError as e:
        print(f"协议错误 {e.code}: {e.message}", file=sys.stderr)
        sys.exit(1)

    print("\n示例运行完毕。")


if __name__ == "__main__":
    main()
