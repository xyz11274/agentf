# e2b 沙箱运维工具

## 脚本列表（2026-08-12 合并整理后）

| 脚本 | 用途 |
|------|------|
| `sandbox_toolkit.py` | 查询和清理远端 e2b 沙箱残留（list/kill/kill-all/prune/run） |
| `sandbox_diag.py` | 沙箱诊断工具合集 — 20 个历史诊断脚本的合并入口（见下表） |
| `template_admin.py` | CubeSandbox 模板运维合集 — 3 个历史模板脚本的合并入口（见下表） |
| `zero_cred_daemon_demo.py` | 零凭据 daemon 演示（凭据由 daemon 内部从 config 加载） |

### template_admin.py 合并说明（2026-08-12）

原 3 个模板运维脚本（REST 接口验证 / 增量快照创建 / 预热模板创建）样板代码重复
（config 加载 / env 设置 / ssl patch / envd url patch / REST 请求封装），已合并为
单一入口 `template_admin.py`，按子命令调度：

| 子命令 | 原脚本 | 用途 |
|--------|--------|------|
| `rest-check` | template_rest_check.py | 模板 REST 接口可用性验证（纯 HTTP 不建沙箱） |
| `snapshot` | template_snapshot_create.py | 增量快照模板创建（REST snapshot 通道，含 --hash-manifest 模式） |
| `warm` | template_warm_create.py | 预热模板创建（REST + CLI commit 双通道） |

```bash
# REST 接口可用性验证
python3 scripts/e2b/template_admin.py rest-check

# 增量快照创建（上传 manifest 资产 + 固化快照，stdout 输出 snap-xxx）
python3 scripts/e2b/template_admin.py snapshot --manifest resource/agent-packages/outfit-assistant/manifest.jsonc

# 计算 manifest includes 内容 hash8（供 C++ preload / 评测 hash 检测）
python3 scripts/e2b/template_admin.py snapshot --hash-manifest <manifest.jsonc>

# 预热模板创建
python3 scripts/e2b/template_admin.py warm
```

### sandbox_diag.py 合并说明（2026-08-12）

原 20 个一次性诊断/验证脚本（DNS 排查、pip 安装验证、凭据冒烟、快照验证等）样板代码
高度重复（读 config → 设 env → ssl patch → 建/连沙箱 → envd url patch → 跑命令 →
kill），已合并为单一入口 `sandbox_diag.py`，按子命令调度：

| 子命令 | 原脚本 | 用途 |
|--------|--------|------|
| `creds` | credentials_smoke.py | 凭据冒烟：config 加载 key → Sandbox.create |
| `nokey` | nokey_sandbox.py | 无凭据创建实验 |
| `deps` | sandbox_deps_check.py | 沙箱预装依赖检查 |
| `dns-deep` | sandbox_dns_deep.py | DNS 根因深挖 + 清华源评估 |
| `dns-fix-verify` | sandbox_dns_fix_verify.py | 覆写 resolv.conf 修复验证 |
| `dns-reach` | sandbox_dns_reach_diag.py | netns 内 DNS server 可达性 |
| `envd-vs-cubecli` | sandbox_envd_vs_cubecli_diag.py | envd vs cubecli 诊断 |
| `hosts-dotenv` | sandbox_hosts_dotenv.py | /etc/hosts 方案 → pip install dotenv |
| `hosts-verify` | sandbox_hosts_verify.py | /etc/hosts 注入域名→IP 验证 |
| `id-diag` | sandbox_id_diag.py | root vs user 身份 DNS 对比 |
| `netdiag` | sandbox_netdiag.py | 沙箱网络能力诊断 |
| `pip-dotenv` | sandbox_pip_dotenv.py | pip install python-dotenv 实测 |
| `pip-check` | sandbox_pip_check.py | pip 安装多源试错（default/tuna/custom 三档，--config/--package/--source/--index-url） |
| `pip-ip-fixed` | sandbox_pip_ip_fixed.py | pip IP 直连绕开 DNS（修正版） |
| `pip-ip-verify` | sandbox_pip_ip_verify.py | pip IP 直连绕开 DNS |
| `pip-local-whl` | sandbox_pip_local_whl.py | pip 装本地 wheel（修文件名） |
| `resolv-env` | sandbox_resolv_env_verify.py | RESOLV_CONF 环境变量验证 |
| `create-retain` | _sandbox_diag_create.py | 建沙箱留作 cubecli exec 对比 |
| `snapshot-verify` | snapshot_verify_stepwise.py | 完整链路 v5（credentials→上传→setup.sh→import） |
| `weather-verify` | verify_snapshot_weather.py | 快照 weather.py 路径/内容/执行验证 |

```bash
# 例：DNS 深挖诊断
python3 scripts/e2b/sandbox_diag.py dns-deep

# 例：连接现有沙箱的诊断（默认连历史诊断沙箱，可用 --sandbox-id 覆盖）
python3 scripts/e2b/sandbox_diag.py dns-reach --sandbox-id <id>
```

## perf_cold_start.py（位于 tests/perf/）

e2b 沙箱冷启动性能基准已归入统一测试目录 `tests/perf/`，不在本目录：
`tests/perf/perf_cold_start.py`

测量 e2b 沙箱冷启动耗时（对应 daemon 侧 `create_latency`），分两段：
① **无上传基线**——纯 SDK create + 首命令 echo，完全不上传任何文件；
② **完整冷启动**——`e2b_lifecycle.py create`（SDK 创建 + HTTP server + 健康检查）
+ `e2b_sync.py upload`（harness/skills 同步到远端）。
完整冷启动超过阈值（默认 15s）判定为回归，退出码 1。

已挂载到 `scripts/tests/regression_test.sh` Step 5（e2b 可用时自动执行）。

```bash
source frontend/bridge/.venv_unix/bin/activate
./tests/perf/perf_cold_start.py                 # 默认阈值 15s
./tests/perf/perf_cold_start.py --threshold 10  # 更严格阈值
./tests/perf/perf_cold_start.py --skip-baseline # 仅测完整冷启动
./tests/perf/perf_cold_start.py --keep          # 测试后保留沙箱
```

典型输出（2026-08-01 实测）：

```
[perf] 无上传基线: create=0.2s 首命令=0.4s 合计=0.6s ✅
[perf] e2b_lifecycle create: 1.4s ✅
[perf] skills upload: 1.9s ✅ (44 files, 207KB)   ← 文件数+总大小（已排除缓存）
[perf] cleanup kill: 0.6s ✅
```

## sandbox_toolkit.py

查询和清理 e2b 沙箱，常用于测试后检查是否有沙箱残留。

### 环境变量

| 变量 | 说明 | 默认值 |
|------|------|--------|
| `E2B_API_URL` | e2b API 地址 | `http://218.245.96.148:30030` |
| `E2B_API_KEY` | e2b API 密钥 | `e2b_000000000` |
| `SSL_CERT_FILE` | CA 证书路径（自签名证书时必填） | 自动从 `docs/env/e2b_CA.pem` 读取 |

### 用法

```bash
# 列出所有运行中的沙箱
E2B_API_KEY="e2b_000000000" \
SSL_CERT_FILE="$(pwd)/docs/env/e2b_CA.pem" \
python3 scripts/e2b/sandbox_toolkit.py list

# 列出所有沙箱（含已暂停）
python3 scripts/e2b/sandbox_toolkit.py list --all

# 查看详细信息
python3 scripts/e2b/sandbox_toolkit.py list --verbose

# 清理指定沙箱
python3 scripts/e2b/sandbox_toolkit.py kill <sandbox_id>

# 清理所有运行中的沙箱（测试后收尾）
python3 scripts/e2b/sandbox_toolkit.py kill-all --force

# 清理运行超过 30 分钟的沙箱（一键清理残留）
python3 scripts/e2b/sandbox_toolkit.py prune --older-than 30 --force
```

### 典型使用场景

**测试后检查残留**：
```bash
python3 scripts/e2b/sandbox_toolkit.py list
# No sandboxes found.  ← 干净
```

**发现有残留时清理**：
```bash
python3 scripts/e2b/sandbox_toolkit.py prune --older-than 5 --force
```
