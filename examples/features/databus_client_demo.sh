#!/usr/bin/env bash
#
# DataBus CLI 工具演示 - examples/features
#
# 展示如何从 Shell 脚本通过 CLI 工具访问 AgentFlow DataBus。
# 本示例是 databus_example.cpp（C++ 内部访问）和 databus_client_demo.py 的 Shell 版本。
#
# 运行前提：
#   - AgentFlow Worker 正在运行并已设置 AF_DATABUS_UDS_FILE 环境变量
#   - src/runtime/databus/clients/cli/databus_cli.py 可访问
#   - Python 3.6+
#
# 用法：
#   ./databus_client_demo.sh
#   AF_DATABUS_UDS_FILE=/tmp/xxx.sock ./databus_client_demo.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CLI="python3 ${PROJECT_ROOT}/src/runtime/databus/clients/cli/databus_cli.py"

# 检查连接配置
if [[ -z "${AF_DATABUS_UDS_FILE:-}" ]]; then
    echo "错误: 请设置 AF_DATABUS_UDS_FILE 环境变量" >&2
    echo "示例: AF_DATABUS_UDS_FILE=/tmp/agentflow_session_xxx.sock ./databus_client_demo.sh" >&2
    exit 1
fi

echo "DataBus CLI 工具演示"
echo "===================="
echo "Socket: ${AF_DATABUS_UDS_FILE}"
echo ""

# ── 基础读写 ──────────────────────────────────────────────────────────────────
echo "[1] 基础读写"

$CLI -ns user put "request_id" "req-001"
$CLI -ns user put "retry_count" "1"

req_id=$($CLI -ns user --raw get "request_id")
echo "    request_id = ${req_id}"

retry=$($CLI -ns user --raw get "retry_count")
echo "    retry_count = ${retry}"

# ── 检查与删除 ────────────────────────────────────────────────────────────────
echo ""
echo "[2] 检查与删除"

if $CLI -ns user contains "request_id" >/dev/null 2>&1; then
    echo "    request_id 存在"
fi

$CLI -ns user remove "request_id"
if ! $CLI -ns user contains "request_id" >/dev/null 2>&1; then
    echo "    request_id 已删除"
fi

# ── 对话隔离 ──────────────────────────────────────────────────────────────────
echo ""
echo "[3] 对话隔离（模拟并行对话）"

$CLI -ns user -d "dial-A" put "skill_result" "技能 A 的结果"
$CLI -ns user -d "dial-B" put "skill_result" "技能 B 的结果"

result_a=$($CLI -ns user -d "dial-A" --raw get "skill_result")
result_b=$($CLI -ns user -d "dial-B" --raw get "skill_result")
echo "    dial-A skill_result = ${result_a}"
echo "    dial-B skill_result = ${result_b}"

# ── 框架通信 ──────────────────────────────────────────────────────────────────
echo ""
echo "[4] 框架通信（sys 命名空间）"

$CLI -ns sys put "response" "节点执行完毕"
response=$($CLI -ns sys --raw get "response")
echo "    sys.response = ${response}"

# ── 发布事件 ──────────────────────────────────────────────────────────────────
echo ""
echo "[5] 发布事件"

$CLI -ns sys publish "agent.event" '{"type": "demo_complete"}'
echo "    已发布 agent.event 事件"

echo ""
echo "演示完毕。"
