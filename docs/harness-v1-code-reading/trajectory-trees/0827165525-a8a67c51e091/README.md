# Harness v1 真实运行轨迹树

本目录将真实会话 `0827165525-a8a67c51e091` 投影为一张可直接查看的运行轨迹树：

- [trajectory-tree.svg](./trajectory-tree.svg)：主图，使用 VS Code 或浏览器直接打开，可无损缩放。
- 原始 SessionLog：`trace_output/sessionlogs/0827165525-a8a67c51e091.jsonl`。
- 原始 Python Span：`trace_output/python/python_spans_0827165525-a8a67c51e091.ndjson`。
- 成功产物：`examples/agent/outfit-assistant/outputs/harness-trajectory-final-20260827d.html`。

## 这张图如何读取

主干是 `session.start → Coordinator #1 → skill.dispatch`。调度点一次分叉出
`outfit-stylist`、`forge-image-gen`、`video-gen` 三个并发分支；三条分支全部产生唯一
`skill.end` 后，在 `batch.result` 汇合，再由 Coordinator #2 生成最终答复，最后形成
`session.end → client.delivery`。

图中的每个 “LLM Round” 都折叠了三个原始事件：

```text
llm.request → llm.transport.request → llm.response
```

Tool 则按 `tool_call_id` 将 `tool.call` 与 `tool.result` 配对。同轮并行 Tool 放在同一个
LLM Round 内。重复的 `artifact.persist status=no_uploadable_files` 做了计数聚合，但成功上传的
HTML 保留了文件名、大小与 SHA256。

## 为什么严格来说不是普通树

这次执行包含一个 fork/join：三个 Skill 共享同一调度父节点，并共同汇入同一个
`batch.result`。因此原始结构是带汇合节点的有向无环图（DAG），而不是每个节点只有一个父节点的
严格树。图中采用“树形展开 + 显式 JOIN”的可视化投影。后续设计 Protobuf 树结构时，不应丢掉：

- `batch_id`：标识共同的并发批次；
- `exec_skill_key`：标识独立分支；
- `primary_parent_event_id` 与 `causal_parent_event_ids`：保存主父边和多因果边；
- fork/join 节点：保证并发执行能够精确回放，而不是被错误串行化。

## 两种成功必须分开

三个 `skill.end` 都是 `completed`，`batch.result.status=true`。它们表示框架成功启动并收回了
三个 Executor 结果，并不表示三个业务目标都完成：

- `outfit-stylist`：业务成功，联网天气查询和 HTML 产物都可回放；
- `forge-image-gen`：Executor 正常结束，但生成脚本因缺少 Provider key 失败，无 PNG；
- `video-gen`：Executor 正常结束，但视频脚本因缺少 key 失败，无 MP4。

这个区分对后续轨迹树评测、无效 Loop 识别和结束条件优化非常关键。
