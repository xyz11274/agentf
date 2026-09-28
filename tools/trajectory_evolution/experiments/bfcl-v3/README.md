# BFCL v3 离线验收

这是开源 BFCL v3 的第二把验收尺，复用 business-simulator 的 AgentFlow
coordinator、skill executor 和自进化服务。固定数据划分为 train 400、GEPA
search 150、selection 50、最终 holdout 200；产物只写入
`trace_output/bfcl-v3`。

不懂代码时，在仓库根目录执行：

```bash
tools/trajectory_evolution/experiments/bfcl-v3/run_bfcl_v3.sh fresh
```

断点继续用 `resume`，查看状态用 `status`，启动前门禁用 `check`。流程会依次
完成训练、GEPA 搜索/选择、holdout 和发布；每一段都有检查点，重启不会复制
业务产物。运行状态在 tmux 会话 `bfcl-v3` 中，查看：

```bash
tmux attach -t bfcl-v3
```
