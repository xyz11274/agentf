#!/usr/bin/env bash
# generate.sh 客户端：通过统一协议收集执行事件并接收策略建议。
# 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
# 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
# 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

# 可选开发工具；部署后的 Agent 和 C++ 服务均不需要运行它。
set -euo pipefail
if [[ $# -ne 2 || ! "$1" =~ ^[a-z][a-z0-9-]*$ ]]; then
  echo 'Usage: bash clients/generate.sh <generator-language> <new-output-directory>' >&2
  exit 2
fi
if [[ -e "$2" ]]; then
  echo 'Output must be a new directory; existing files are not overwritten.' >&2
  exit 2
fi
client_source_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
generator_command="${OPENAPI_GENERATOR_CLI:-openapi-generator-cli}"
if ! command -v "$generator_command" >/dev/null 2>&1; then
  echo 'Install OpenAPI Generator CLI, or set OPENAPI_GENERATOR_CLI to its executable path.' >&2
  exit 2
fi
exec "$generator_command" generate -i "$client_source_dir/../api/openapi.json" -g "$1" -o "$2"
