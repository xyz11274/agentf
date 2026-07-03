# 命名约定问题

## 状态

**未解决** — 当前存在两套并行的命名规范，没有统一的权威定义。

## 问题

Agent 的名称在三个地方出现，各自使用不同的命名格式：

| 位置 | 格式 | 示例 |
|------|------|------|
| **目录名**（文件系统） | 小写下划线 | `harness_agent/` |
| **`package.json` `name`**（逻辑标识符） | 小写连字符 | `harness-agent` |
| **`agent.json` `name`**（展示名） | PascalCase | `HarnessAgent` |

## 影响

bridge 每次需要用 `agent_id`（连字符）去定位磁盘上的目录时，都必须做 `-` → `_` 转换。如果漏掉某个路径，就会报"Failed to load agent from: ..." 错误。

## 当前缓解方案

在 `frontend/bridge/routes/agents.py` 的 `load_agent` 中，当 `body.package_path` 未提供时，对 `agent_id` 做 `replace("-", "_")` 转换以匹配磁盘目录名。

## 三种可能的统一方向

| 方向 | 改动量 | 影响 |
|------|--------|------|
| A: **目录名改为连字符**，与 `package.json name` 一致 | 大：所有 agent 目录重命名 + 所有 `working_dir` + README + 构建脚本 + C++ 源码路径 | 无需任何转换 |
| B: **`package.json name` 改为下划线**，与目录名一致 | 小：只改 10 个 `package.json` | daemon 的 agent_id 变成下划线，前端显示可能不美观 |
| C: **桥接层统一转换**（当前方案） | 小：维护 `-` → `_` 映射 | 需要确保所有路径代码都覆盖到 |

## 当前结论

采用方向 C 作为实用方案，直到有明确的统一决定。
