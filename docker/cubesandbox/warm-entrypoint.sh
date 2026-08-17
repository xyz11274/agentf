#!/bin/bash
# 预热模板 entrypoint — "envd 后台 + HTTP server 前台" 模式（对齐官方 cube-entrypoint.sh 语义）
#
# 背景（2026-08-09 确认）：sandbox-code:latest 的 /init 是 s6-overlay，
# 负责拉起 envd(:49983) 等 service。本镜像为预跑 sandbox_http_server.py，
# 若直接替换 ENTRYPOINT 为 HTTP server，envd 将不被拉起 → 模板创建 probe 失败。
# 因此这里手动在后台拉起 envd，前台运行 HTTP server（官方 BYOI 文档 §4 模式）。
#
# 注意（2026-08-09 实测修正）：官方文档的 `-isnotfc` flag 仅新版 envd（2026.16）支持，
# 本环境 sandbox-code 内置 envd 0.2.11 **不支持**（`flag provided but not defined`），
# 且 0.2.11 无 MMDS 查找逻辑（不带 -isnotfc 实测 /health 204 正常），故此处不带该 flag。
# 与先例 agentfoundry-sandbox:cube 的 s6 service（`exec /usr/bin/envd`）一致。

set -euo pipefail

# 1. 后台拉起 envd（平台协议端点 :49983，模板就绪 probe / SDK 通信依赖）
/usr/bin/envd -port "${ENVD_PORT:-49983}" >/var/log/envd.log 2>&1 &

# 2. 前台运行 HTTP server（:8080，agentflow 工具执行端点 /exec）
exec python3 /opt/sandbox/http_server/sandbox_http_server.py \
    --port "${SANDBOX_HTTP_PORT:-8080}" --host 0.0.0.0
