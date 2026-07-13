# Debug 工具集

本目录包含 AgentFlow daemon 调试相关工具。

## 工具列表

| 工具 | 用途 | 用法 |
|------|------|------|
| `trace.sh` | 按 trace_id 查询全链路日志 | `./trace.sh trc_xxx` |
| `trace.sh -l` | 列出最近的 trace_id | `./trace.sh -l` |

## 快速开始

```bash
# 从默认日志路径查询
./trace.sh trc_1524029646_1

# 指定日志文件
./trace.sh trc_1524029646_1 /tmp/agentflowd_test.log

# 查看最近的 trace
./trace.sh -l
```

## 输出示例

```
━━━ AgentFlow Trace: trc_1524029646_1 ━━━
  日志文件: /path/to/agentflowd.log
  时间范围: 23:51:38 → 23:52:02  (~24000ms)

① 用户输入
  23:51:38 [USER_INPUT] agent=outfit-assistant content_preview="hi"
② 技能启动
  23:51:38 [LAUNCH_SKILL] Created skill task: ... for skill: dummy_pre_skill
③ 消息交换
  23:51:43 [HANDLE_DATA_EXCHANGE] dispatcher trace=trc_1524029646_1
  23:51:43 [HANDLE_DATA_EXCHANGE] coordinator trace=trc_1524029646_1
④ 最终响应
  23:51:46 [COORDINATOR_SEND] Enqueueing response ...
⑤ 摘要
  23:51:46 [E2E_SUMMARY] dialogue=xxx trace=trc_xxx response_len=545
⑥ 清理
  23:51:46 [CLEANUP_DIALOGUE] Cleaned 15 sys keys
─── 共 N 条日志, 耗时 ~24000ms ───
```
