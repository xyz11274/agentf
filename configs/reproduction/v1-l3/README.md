# 固定复现：真实 AgentFlow v1 / L0 与 L3

在项目根目录、`agentflow-dev` 环境执行：

```bash
python scripts/reproduce_v1_l3.py --check
python scripts/reproduce_v1_l3.py
```

第一条只检查冻结文件、数据集、依赖、端口与凭据是否配置，不调用模型。
第二条使用原来的 15 道 holdout 题，对固定 L0 和 L3 各执行一次，共 30 次真实 Agent 执行，会消耗 API 额度。
程序启动原有 agentflowd 与 Bridge，通过 Bridge 已接入的任务入口调用 v1；不实现另一份 Agent，
不重新运行 GEPA、不学习、不修改记忆，也不切换主 Agent 的策略。

## 固定内容

- 策略：2026-09-14 最终验收的 L3 候选及同次实验的 L0；不从当前记忆库重新编译。
- 模型：文本与评分为 `deepseek-v4-flash`，文本 temperature=0、thinking=disabled。
- 文本接口：火山 `/api/coding/v3`；图片使用 `/api/plan/v3` 的 `doubao-seedream-5.0-lite`。
- 视频接口保留当时的模型与配置；相关题目仍按原标准检查不可用时的失败处理。
- 任务、判分规则、模型评审提示词、时限、逐题单并发均复用原验收配置。
- `manifest.json` 保存版本、运行参数和输入文件 SHA-256。输入或相关源码改变时停止，避免把改过的环境算作同一版本复现。

本目录针对当前工作区已有的真实实验，依赖 `build/trajectory-evolution/v1-coldstart-input/` 中保留的部署材料，
以及 `trace_output/v1-l4-coldstart/` 中冻结的数据集和版本。请保留这些目录；这不是重新从空库训练的入口。

## 凭据

默认从 `examples/agent/outfit-test-agent/.env` 读取凭据；终端已有的同名凭据优先。
API URL、模型和运行参数由冻结配置确定，不随 `.env` 的非凭据字段改变。
密钥不复制到 manifest、生成配置或报告，也不进入命令行参数。

指定其他凭据文件：

```bash
python scripts/reproduce_v1_l3.py --env-file /path/to/test.env
```

测试使用 loopback 的 18811、18812 端口；已有服务占用时退出，不停止它们。同一工作区禁止同时启动两次复现。

## 结果

默认输出到 `trace_output/v1-l3-reproduction/<UTC时间>-<随机后缀>/`，每次使用新目录。
也可以指定尚不存在的目录：

```bash
python scripts/reproduce_v1_l3.py --output-root trace_output/v1-l3-reproduction/run-001
```

查看 `report.md` 的逐题对照、`report.json` 的完整评分与轨迹、`results.jsonl` 的已完成试次。
程序结束时输出两侧通过题数、无效 loop 均值和 Agent 平均耗时。
退出码 0=全部通过，1=存在已评分的失败题，3=评分/执行未完整结束，2=配置或输入错误。
L0 自身失败也可能使退出码为 1，应结合两侧成绩判断，而不是隐藏该题。

历史实验的无效 loop 降幅为 50%；本次成绩以新报告为准。即使温度为 0，远程模型及工具服务也可能波动。

## 离线策略包

原验收目录下的 `published-version.json` 由修复后的组件按真实发布记录自动补导出。
它包含完整 artifact、版本 ID 和原验收 gate，可通过组件的 `policy_version_file` 加载。
版本属于 `agentflow-v1-outfit` / `v1-cold-bravo`；离线运行仍保留这个应用、用户和边界授权。
冻结包不需要原 memory 数据库，加载后关闭学习与后台评测，保留正常采集。
