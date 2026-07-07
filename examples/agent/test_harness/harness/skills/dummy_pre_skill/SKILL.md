# dummy_pre_skill — 前置 hook 占位

## 行为
- 接收用户原始输入（AF_INPUT）
- 不修改、不写 DataBus、不输出 next_skills
- framework 自动 forward 给 coordinator task
