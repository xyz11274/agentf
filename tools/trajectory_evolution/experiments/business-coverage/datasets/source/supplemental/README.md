# 补充合成训练数据汇总

6批，272个会话、563轮。`train.jsonl` 保留所有源字段；`manifest.json` 记录各批来源及行范围。不包含原train400、native复制或重放输出，不自动改变正在运行的数据集。

使用val/holdout反馈生成的批次保留来源，后续同题成绩属于定向修复复测。

更新汇总：`python3 tools/trajectory_evolution/experiments/business-coverage/collect_supplemental_data.py`。
