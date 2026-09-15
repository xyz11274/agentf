# scripts/tests — 测试脚本目录

> 本目录集中存放可执行测试脚本(bash/py)。**唯一全量回归入口是 `regression_test.sh`**;
> 其余脚本是特定场景的专项测试,**不要在日常开发中逐个手动跑**——先按下方决策表选。
>
> 维护约定:新增脚本必须登记到本文件速查表;删除脚本前先确认不被 `regression_test.sh` 引用。

## 脚本速查表

| 脚本 | 场景 | 用法 | 依赖 | 是否被 regression 引用 |
|------|------|------|------|------------------------|
| `regression_test.sh` | **全量回归**(fix → build → unit → e2e → frontend vitest → python bridge pytest) | `scripts/tests/regression_test.sh [--skip-build \| --skip-fix \| --unit-only \| --e2e-only]` | 本地编译链 + 测试二进制 | —(自身是入口) |
| `e2b_fork_smoke.sh` | e2b 沙箱 fork 生命周期 smoke(create → upload → snapshot → fork → 验证继承) | `E2B_API_KEY=xxx bash scripts/tests/e2b_fork_smoke.sh` | E2B API + `docs/env/e2b_CA.pem` | ✅ 被 regression 的 e2b smoke 段调用 |
| `remote_e2e_test.sh` | 已部署远程 bridge 的 REST/SSE 验证(**不 build、不自启动服务**) | `./scripts/tests/remote_e2e_test.sh --bridge-url https://preprod:8199` | 远程 endpoint | ⚠️ 仅提示引用(regression 输出指引用此脚本) |
| `run_perf.sh` | 性能测试三层策略(L1 mock LLM / L2 真实 LLM 简答 / L3 真实业务) | `./scripts/tests/run_perf.sh [layer1\|layer2\|layer3\|--help]` | `tests/perf/run_concurrent_perf.py` + 测试环境 key | ❌ |
| `test_skills_upload_e2e.sh` | skill 上传/列举/删除 JSON-RPC E2E | `./scripts/tests/test_skills_upload_e2e.sh` | 已运行 daemon(`AF_HOST/AF_PORT`) | ❌ |
| `test_start_bridge.py` | `start_bridge.sh` 启动配置的静态断言(dev reload 范围 / prod 无 reload) | `pytest scripts/tests/test_start_bridge.py` | pytest | ❌(由 CI/pytest 单独收集) |
| `test_secret_scanner.py` | 厂商无关的硬编码凭据扫描器单测 | `pytest scripts/tests/test_secret_scanner.py` | pytest | ❌（由 Credential Safety CI 单独执行扫描器） |

## 决策表:什么场景跑哪个

| 你的目标 | 运行 |
|---------|------|
| 提交前/日常全量回归(改动了 C++/bridge/节点) | **`regression_test.sh`**(唯一入口) |
| 只跑单元测试 | `regression_test.sh --unit-only` |
| 只跑 E2E | `regression_test.sh --e2e-only` |
| 验证 e2b 沙箱 fork 能力 | `e2b_fork_smoke.sh`(或回归中的 e2b smoke 段) |
| 验证**已部署**的远程 bridge 可用性 | `remote_e2e_test.sh --bridge-url URL` |
| 压测并发/性能 | `run_perf.sh layer1/2/3` |
| 验证 skill 上传接口 | `test_skills_upload_e2e.sh` |
| 验证源码凭据门禁 | `pytest scripts/tests/test_secret_scanner.py` 后运行 `python3 scripts/quality/scan_secrets.py --tracked` |
| 只验证 start_bridge.sh 配置 | `test_start_bridge.py` |
| `test_secret_scanner.py` | 厂商无关的硬编码凭据扫描器单测 | `pytest scripts/tests/test_secret_scanner.py` | pytest | ❌（由 Credential Safety CI 单独执行扫描器） |

## 合并分析结论(2026-08-07)

**结论:不做脚本合并,保留现状。** 7 个脚本场景互不重叠,强行合并会引入参数分支与依赖耦合,且 `regression_test.sh` 是稳定入口不应改动:

- `e2b_fork_smoke.sh` 是 regression 的**组成部分**(被引用),不能删/并;
- `remote_e2e_test.sh` 是 regression 的**互补场景**(远程 vs 本地),regression 已显式指引它;
- `run_perf.sh` / `test_skills_upload_e2e.sh` / `test_start_bridge.py` / `test_secret_scanner.py` 是独立专项,并入 regression 会拖慢全量回归且职责混乱。

"脚本太多不知道运行哪个"的根因是**缺少入口说明**,由本文件的速查表 + 决策表解决,而非物理合并。

## 维护约束

- 🔒 **`regression_test.sh` 是稳定入口,不要随意重构**:它承载全量回归的步骤编排与 PASS/FAIL 计数,修改前必须跑通至少 `--unit-only` 验证。
- 新增测试脚本 → 更新速查表 + 决策表;若应纳入全量回归,在 `regression_test.sh` 对应阶段注册调用。
- 删除脚本前 `grep` 确认无引用(`regression_test.sh` 内部调用、CI 配置、其他脚本)。
