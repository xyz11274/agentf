#!/bin/bash
# 沙箱内环境初始化脚本 — 被 e2b_template_build.py 和 Dockerfile 引用
#
# 用途：在沙箱容器内安装 Python 依赖，使技能脚本能使用第三方库。
# 当前沙箱内脚本仅使用 Python stdlib，无需额外依赖。
# 当未来技能需要 pip 包时，在此文件添加 pip install 命令。
#
# 设计原则：
# - 保持最小化，只装沙箱内运行所需的依赖（不是宿主机 node 脚本的依赖）
# - 宿主机 node 脚本的依赖通过 setup.sh + requirements.txt 管理
# - 沙箱内依赖与宿主机依赖分离，避免镜像膨胀
#
# 调用方式（由 e2b_template_build.py 在构建模板时调用）：
#   Template().add_commands(["bash /opt/sandbox/sandbox-setup.sh"])
#
# 或在 Dockerfile 中：
#   COPY docker/sandbox/sandbox-setup.sh /opt/sandbox/
#   RUN bash /opt/sandbox/sandbox-setup.sh

set -euo pipefail

echo "[sandbox-setup] Starting sandbox environment setup..."

# ── 当前沙箱内脚本仅使用 Python stdlib，无需 pip 安装 ──
# 当未来需要安装依赖时，在此添加：
#
# pip install --no-cache-dir \
#     requests \
#     numpy \
#     ...

echo "[sandbox-setup] All sandbox scripts use Python stdlib only, no pip deps needed."
echo "[sandbox-setup] Sandbox environment setup complete."