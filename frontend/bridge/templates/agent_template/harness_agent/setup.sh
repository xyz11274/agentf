#!/bin/bash
# Harness Agent 沙箱环境初始化（setup.sh）— 用户感知脚本，随包上传沙箱
#
# 定位（对齐 docs/design/agent-template-delta.md §5.4 + sandbox-warm-template-oci.md）：
# 本脚本随 agent 包上传到沙箱内（manifest includes 已含 setup.sh），由沙箱预热链路
# （template_admin.py snapshot）执行。**不建 .venv**——沙箱本身是单 agent 隔离环境，
# 直接用系统 python；系统 python 就是沙箱内命令 `python3` 的 PATH 默认解析。
#
# 与 setup-host.sh（宿主版）分离：宿主是多 agent 共享，需要 .venv 隔离（wrapper 检测
# .venv + sentinel 契约 §3.5）；沙箱单 agent 隔离，直接系统 python，不需要 .venv 套娃。
#
# 做三件事：
#   1. 安装 requirements.txt 依赖到系统 python（两档 fallback：外网 pip → curl --resolve IP workaround）
#   2. agentflow-runtime 用 PYTHONPATH 注入（源码已由宿主 --upload-dir 上传，无需 pip install）
#   3. PYTHONPATH 写 ~/.profile 持久化（沙箱 commands.run 是 bash -l -c，自动加载 profile）
#
# 用法（沙箱预热）：
#   bash setup.sh                  # 沙箱内初始化（由 template_admin.py snapshot 驱动）
#
# 环境变量：
#   AF_RUNTIME_SRC — agentflow-runtime 源码目录（默认 $SCRIPT_DIR/packages/agentflow-runtime/src）
#   PYPI_IP        — pypi.org 的 IPv4（绕 DNS workaround 用，默认 151.101.128.223，CDN 换 IP 时复核）
#   HOSTED_IP      — files.pythonhosted.org 的 IPv4（绕 DNS workaround 用，默认 151.101.0.223）

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

REQ_FILE="$SCRIPT_DIR/requirements.txt"

# CDN IP 可被环境变量覆盖（运维侧已知 CDN 换 IP 时覆盖，详见 sandbox-warm-template-oci.md §2.7）
PYPI_IP="${PYPI_IP:-151.101.128.223}"
HOSTED_IP="${HOSTED_IP:-151.101.0.223}"

# ── 1. 安装 requirements.txt 依赖到系统 python（两档 fallback）──────────────────────
# 实测（2026-08-11，详见 docs/bugs/20260811_sandbox_dns_unreachable.md）：
#   - 沙箱 netns 内 /etc/resolv.conf 指向的 169.254.254.53 链路本地 DNS 不可达
#   - 但沙箱外网 IP 完全可达（8.8.8.8:53 TCP 通、PyPI IP 直连 HTTP 200）
#   - /etc/resolv.conf 只读、/etc/hosts root only、RESOLV_CONF glibc 没认 → 改 DNS 路径全堵
# 两档策略：
#   ① 外网 pip（运维修好 DNS 后这条直接通，主路径）
#   ② curl --resolve IP 直连 workaround（DNS 不通时绕开 resolver）
# 沙箱已预装 yaml/requests/urllib3（实测沙箱内 import 成功），requirements.txt 声明的依赖均覆盖
# .env 加载策略：python-dotenv 首选，未装时兜底走 stdlib 零依赖解析器
#（agentflow_runtime.common.env_loader）——沙箱装不上 dotenv 也能跑（详见 docs/bugs/20260811_sandbox_dns_unreachable.md）
install_deps() {
    if [ ! -f "$REQ_FILE" ]; then
        echo "ℹ️  无 requirements.txt，跳过依赖安装"
        return 0
    fi
    echo "📦 尝试安装 requirements.txt 依赖到系统 python ..."

    # ① 外网 pip（运维修好 DNS 后主路径）
    if pip install -r "$REQ_FILE" --quiet --break-system-packages 2>/dev/null \
        || pip install -r "$REQ_FILE" --quiet 2>/dev/null; then
        echo "✅ 外网 pip 安装完成（沙箱 DNS 可达）"
        return 0
    fi

    echo "⚠️  外网 pip 失败（沙箱 netns 内 DNS 169.254.254.53 不可达），尝试 curl --resolve IP 直连 ..."

    # ② curl --resolve IP 直连 workaround（绕开 DNS resolver）
    #    逐包：拉 PyPI 元数据 → 解析 wheel URL → curl 下载 wheel（保留原始文件名！）→ pip 本地装
    #    pip 25.0.1 校验 wheel 文件名格式（包名-版本-平台tag.whl），改名会报 not a valid wheel filename
    local pkg line url filename install_ok=1
    while IFS= read -r line; do
        # 跳过注释/空行/选项行（-e -- 等）
        case "$line" in
            ""|"#"*|"-"*|*"=="*|*">="*|*"<="*|*";"*) continue ;;
        esac
        pkg="${line%%[[:space:]]*}"
        [ -z "$pkg" ] && continue
        echo "  → $pkg"
        if _install_via_resolve "$pkg"; then
            echo "    ✅ $pkg 装上"
        else
            echo "    ⚠️  $pkg 装失败（curl --resolve 路径）"
            install_ok=0
        fi
    done < "$REQ_FILE"

    if [ "$install_ok" = "1" ]; then
        echo "✅ curl --resolve IP 直连 workaround 装完成"
    else
        echo "⚠️  部分包 curl --resolve 装失败——沙箱预装 yaml/requests/urllib3 不受影响，dotenv 非必需"
    fi
}

# 单包安装：curl --resolve 拉 PyPI 元数据 → 下载 wheel → pip 本地装
# 返回 0 = 装上，1 = 失败
_install_via_resolve() {
    local pkg="$1"
    local meta="/tmp/_${pkg}_meta.json"
    local url_file="/tmp/_${pkg}_url.txt"

    # 1. 拉 PyPI 包元数据（pypi.org 走 IP 直连，绕开 DNS）
    if ! curl -s -m 20 --resolve "pypi.org:443:${PYPI_IP}" \
            -o "$meta" "https://pypi.org/pypi/${pkg}/json"; then
        return 1
    fi

    # 2. python 解析元数据拿 wheel URL（写文件，不嵌套 shell 转义）
    if ! python3 -c "
import json, sys
try:
    d = json.load(open('${meta}'))
    url = d['urls'][0]['url']
    open('${url_file}', 'w').write(url)
    print('URL_SAVED')
except Exception as e:
    print(f'PARSE_FAIL: {e}', file=sys.stderr)
    sys.exit(1)
" 2>/dev/null; then
        return 1
    fi

    # 3. 下载 wheel（files.pythonhosted.org 走 IP 直连，保留原始文件名！pip 校验文件名格式）
    url="$(cat "$url_file")"
    filename="$(basename "$url")"
    if ! curl -s -m 90 --resolve "files.pythonhosted.org:443:${HOSTED_IP}" \
            -o "/tmp/${filename}" "$url"; then
        return 1
    fi

    # 4. pip 本地装 wheel
    pip install "/tmp/${filename}" --quiet --break-system-packages 2>/dev/null
}

install_deps

# ── 2. agentflow-runtime：PYTHONPATH 注入（源码已由宿主上传，无需 pip install）──
# 宿主上传位置：$SCRIPT_DIR/packages/agentflow-runtime/src（template_registry.cpp
# --upload-dir packages/agentflow-runtime:/home/user/workspace/packages/agentflow-runtime）。
# 真实验证（2026-08-11 v4）：PYTHONPATH 指向该 src 后 import agentflow_runtime 成功（AFRT_OK）。
# 持久化：写 ~/.profile——沙箱 commands.run 是 bash -l -c（login shell，实证
# e2b/sandbox_sync/commands/command.py L322），自动加载 profile，后续所有命令
# 的 python3 都能 import agentflow_runtime。setup.sh 仅在沙箱执行（宿主用 setup-host.sh），
# 写 ~/.profile 无宿主污染。
AF_RUNTIME_SRC="${AF_RUNTIME_SRC:-$SCRIPT_DIR/packages/agentflow-runtime/src}"
if [ -d "$AF_RUNTIME_SRC" ]; then
    PROFILE="${HOME}/.profile"
    if grep -qF "$AF_RUNTIME_SRC" "$PROFILE" 2>/dev/null; then
        echo "✅ agentflow-runtime PYTHONPATH 已注入（重复执行跳过）"
    else
        echo "export PYTHONPATH=\"$AF_RUNTIME_SRC:\$PYTHONPATH\"" >> "$PROFILE"
        echo "✅ agentflow-runtime PYTHONPATH 已写入 $PROFILE（bash -l 自动加载）"
    fi
else
    echo "⚠️  agentflow-runtime 源码未上传（$AF_RUNTIME_SRC 不存在）——沙箱内 import agentflow_runtime 将失败"
fi

# ── 3. 校验 .env ──
if [ ! -f "$SCRIPT_DIR/.env" ]; then
    echo ""
    echo "⚠️  未找到 .env 文件"
    echo "   凭据应由宿主环境或本地忽略文件提供；manifest 不得包含 .env"
fi

echo ""
echo "✅ 沙箱环境初始化完成"
echo "  - requirements.txt：best-effort 安装（多数已预装）"
echo "  - agentflow-runtime：PYTHONPATH 注入 ~/.profile（bash -l 自动加载）"
