# Agent Perf Noop — 纯调度性能测试 fixture

零 LLM 调用，仅测 Bridge + Daemon 的 session 调度开销。
用于构建并发性能基线的 Layer 0。

## 行为

- 单 coordinator 节点，但 **不调用 LLM**
- `coordinator_decide.sh` 被覆盖为直接返回固定 JSON
- 响应时间 = 纯调度开销（无 LLM 网络延迟、无模型推理）
- 配合 `run_concurrent_perf.py --agent perf-noop` 使用
