#!/usr/bin/env bash
#
# AgentFlow DataBus CLI 工具基础用法示例
#
# 演示通过命令行工具对 DataBus 进行 put/get/contains/remove/subscribe 操作。
#
# 运行前提：AgentFlow Worker 已启动并设置 AF_DATABUS_UDS_FILE 环境变量。
# 也可以显式指定 socket 路径：
#   SOCKET=/tmp/agentflow_session_xxx.sock ./basic_usage.sh
#
# 依赖：Python 3.6+，以及 src/runtime/databus/clients/python/agentflow_databus.py

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLI="${SCRIPT_DIR}/../databus_cli.py"

# 如果未通过环境变量指定，使用显式 SOCKET
SOCKET="${AF_DATABUS_UDS_FILE:-${SOCKET:-}}"

if [[ -z "${SOCKET}" ]]; then
    echo "错误: 请设置 AF_DATABUS_UDS_FILE 环境变量或 SOCKET 变量" >&2
    exit 1
fi

CLI_CMD="python3 ${CLI} -S ${SOCKET}"

echo "连接到 DataBus: ${SOCKET}"
echo ""

# ─────────────────────────────────────────────────────────────────────────────
echo "=== 基础读写操作 ==="

# 写入字符串值
${CLI_CMD} -ns user put "greeting" "Hello, AgentFlow!"
echo "put greeting: Hello, AgentFlow!"

# 读取值
value=$(${CLI_CMD} -ns user --raw get "greeting")
echo "get greeting: ${value}"

# 写入 JSON 对象
${CLI_CMD} -ns user put "config" '{"debug": true, "level": 5}'
echo 'put config: {"debug": true, "level": 5}'

config=$(${CLI_CMD} -ns user get "config")
echo "get config: ${config}"

# 检查键是否存在
if ${CLI_CMD} -ns user contains "greeting" 2>/dev/null; then
    echo "contains greeting: true"
fi

if ! ${CLI_CMD} -ns user contains "nonexistent_key" 2>/dev/null; then
    echo "contains nonexistent_key: false"
fi

# 删除键
${CLI_CMD} -ns user remove "greeting"
echo "remove greeting"

if ! ${CLI_CMD} -ns user contains "greeting" 2>/dev/null; then
    echo "contains greeting after remove: false"
fi

# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== 命名空间访问 ==="

# sys 命名空间：框架通信
${CLI_CMD} -ns sys put "response" "OK"
echo "sys.response = $(${CLI_CMD} -ns sys --raw get "response")"

# ctx 命名空间：上下文存储
${CLI_CMD} -ns ctx put "history" '["msg1", "msg2"]'
echo "ctx.history = $(${CLI_CMD} -ns ctx --raw get "history")"

# user 命名空间：用户自定义数据
${CLI_CMD} -ns user put "result" '{"status": "completed"}'
echo "user.result = $(${CLI_CMD} -ns user --raw get "result")"

# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== 对话隔离 ==="

DIALOGUE_A="dialogue-001"
DIALOGUE_B="dialogue-002"

# 两个对话写入相同键名，数据互不干扰
${CLI_CMD} -ns user -d "${DIALOGUE_A}" put "skill_result" "Result from dialogue A"
${CLI_CMD} -ns user -d "${DIALOGUE_B}" put "skill_result" "Result from dialogue B"

result_a=$(${CLI_CMD} -ns user -d "${DIALOGUE_A}" --raw get "skill_result")
result_b=$(${CLI_CMD} -ns user -d "${DIALOGUE_B}" --raw get "skill_result")
echo "Dialogue A skill_result: ${result_a}"
echo "Dialogue B skill_result: ${result_b}"

# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== 发布事件 ==="

${CLI_CMD} -ns sys publish "agent.event" '{"type": "start", "agent_id": "demo"}'
echo "已发布 agent.event 事件"

# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "示例运行完毕。"
