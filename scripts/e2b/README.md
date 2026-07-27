# E2B 沙箱运维工具

## 脚本列表

| 脚本 | 用途 |
|------|------|
| `sandbox_list.py` | 查询和清理远端 E2B 沙箱残留 |

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