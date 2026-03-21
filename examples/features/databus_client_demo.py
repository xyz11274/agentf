#!/usr/bin/env python3
"""
DataBus Python SDK 演示 - examples/features

展示如何从外部 Python 进程通过 UDS 访问 AgentFlow DataBus。
本示例是 databus_example.cpp（C++ 内部 DataBus 访问）的多语言对应版。

运行前提：
  - AgentFlow Worker 正在运行并已设置 AF_DATABUS_UDS_FILE 环境变量
  - src/runtime/databus/clients/python/agentflow_databus.py 可访问

用法：
  # Worker 会自动设置 AF_DATABUS_UDS_FILE，直接运行节点脚本时使用：
  python3 databus_client_demo.py

  # 手动测试时显式指定 socket：
  python3 databus_client_demo.py /tmp/agentflow_session_xxx.sock
"""

import os
import sys
import time

# 自动定位 SDK（相对于本文件的路径）
_this_dir = os.path.dirname(os.path.abspath(__file__))
_sdk_dir = os.path.join(_this_dir, "..", "..", "src", "runtime", "databus", "clients", "python")
sys.path.insert(0, os.path.normpath(_sdk_dir))

try:
    from agentflow_databus import DataBusClient, DataBusError, JSONRPCError
except ImportError as e:
    print(f"错误: 无法导入 agentflow_databus 模块: {e}", file=sys.stderr)
    print(f"请确认 SDK 路径正确: {_sdk_dir}", file=sys.stderr)
    sys.exit(1)


def run_demo(client: DataBusClient) -> None:
    print(f"DataBus Python SDK 演示")
    print(f"========================")
    print(f"Socket: {client.socket_path}")
    print(f"Session: {client.session_id}")
    print()

    # ── 基础读写 ────────────────────────────────────────────────────────────
    print("[1] 基础读写")
    client.user().put("request_id", "req-001")
    client.user().put("retry_count", 1)

    req_id = client.user().get("request_id")
    print(f"    request_id = {req_id}")

    # ── 订阅变更 ────────────────────────────────────────────────────────────
    print("[2] 订阅变更")
    changes = []

    def on_retry_change(key: str, value) -> None:
        print(f"    [订阅回调] {key} = {value}")
        changes.append(value)

    sub_id = client.user().subscribe("retry_count", on_retry_change)
    client.user().put("retry_count", 2)
    time.sleep(0.1)
    client.user().put("retry_count", 3)
    time.sleep(0.1)
    client.user().unsubscribe(sub_id)
    print(f"    共收到 {len(changes)} 次变更通知")

    # ── 对话隔离 ────────────────────────────────────────────────────────────
    print("[3] 对话隔离（模拟并行对话）")
    client.user("dial-A").put("skill_result", "技能 A 的结果")
    client.user("dial-B").put("skill_result", "技能 B 的结果")
    print(f"    dial-A skill_result = {client.user('dial-A').get('skill_result')}")
    print(f"    dial-B skill_result = {client.user('dial-B').get('skill_result')}")

    # ── 框架通信 ────────────────────────────────────────────────────────────
    print("[4] 框架通信（sys 命名空间）")
    client.sys().put("response", "节点执行完毕")
    print(f"    sys.response = {client.sys().get('response')}")

    print()
    print("演示完毕。")


def main() -> None:
    if len(sys.argv) > 1:
        client = DataBusClient(sys.argv[1])
    else:
        client = DataBusClient.from_env()

    try:
        with client:
            run_demo(client)
    except DataBusError as e:
        print(f"连接错误: {e}", file=sys.stderr)
        sys.exit(1)
    except JSONRPCError as e:
        print(f"协议错误 {e.code}: {e.message}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
