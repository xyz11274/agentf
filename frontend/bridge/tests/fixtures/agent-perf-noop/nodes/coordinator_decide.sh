#!/bin/bash
# ──────────────────────────────────────────────────────────────────────
# Noop coordinator — 不调 LLM，直接返回固定响应
# 用于测试纯调度开销（Layer 0 基线）
# ──────────────────────────────────────────────────────────────────────

set -euo pipefail

# 输出固定 JSON（匹配 template coordinator 的输出格式）
cat <<'EOF'
{"response": "ok", "next_skills": "", "coordinator_status": "completed"}
EOF
