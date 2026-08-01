# E2B 沙箱运维工具

## 脚本列表

| 脚本 | 用途 |
|------|------|
| `sandbox_list.py` | 查询和清理远端 E2B 沙箱残留 |

## perf_cold_start.py（位于 tests/perf/）

E2B 沙箱冷启动性能基准已归入统一测试目录 `tests/perf/`，不在本目录：
`tests/perf/perf_cold_start.py`

测量 E2B 沙箱冷启动耗时（对应 daemon 侧 `create_latency`），分两段：
① **无上传基线**——纯 SDK create + 首命令 echo，完全不上传任何文件；
② **完整冷启动**——`e2b_lifecycle.py create`（SDK 创建 + HTTP server + 健康检查）
+ `e2b_sync.py upload`（harness/skills 同步到远端）。
完整冷启动超过阈值（默认 15s）判定为回归，退出码 1。

已挂载到 `scripts/tests/regression_test.sh` Step 5（E2B 可用时自动执行）。

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

## sandbox_list.py

查询和清理 E2B 沙箱，常用于测试后检查是否有沙箱残留。

### 环境变量

| 变量 | 说明 | 默认值 |
|------|------|--------|
| `E2B_API_URL` | E2B API 地址 | `http://218.245.96.148:30030` |
| `E2B_API_KEY` | E2B API 密钥 | `e2b_000000000` |
| `SSL_CERT_FILE` | CA 证书路径（自签名证书时必填） | 自动从 `docs/env/e2b_CA.pem` 读取 |

### 用法

```bash
# 列出所有运行中的沙箱
E2B_API_KEY="e2b_000000000" \
SSL_CERT_FILE="$(pwd)/docs/env/e2b_CA.pem" \
python3 scripts/e2b/sandbox_list.py list

# 列出所有沙箱（含已暂停）
python3 scripts/e2b/sandbox_list.py list --all

# 查看详细信息
python3 scripts/e2b/sandbox_list.py list --verbose

# 清理指定沙箱
python3 scripts/e2b/sandbox_list.py kill <sandbox_id>

# 清理所有运行中的沙箱（测试后收尾）
python3 scripts/e2b/sandbox_list.py kill-all --force

# 清理运行超过 30 分钟的沙箱（一键清理残留）
python3 scripts/e2b/sandbox_list.py prune --older-than 30 --force
```

### 典型使用场景

**测试后检查残留**：
```bash
python3 scripts/e2b/sandbox_list.py list
# No sandboxes found.  ← 干净
```

**发现有残留时清理**：
```bash
python3 scripts/e2b/sandbox_list.py prune --older-than 5 --force
```