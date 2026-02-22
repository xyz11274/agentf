# 贡献指南

感谢你为 AgentFlow 做出贡献。

## 开始之前

1. Fork 仓库并创建分支：`feature/<name>` 或 `fix/<name>`
2. 本地构建与测试：
   - `./build.sh build`
   - `./build.sh test`
3. 修改完成后同步更新相关文档（尤其 `publish/` 与 `docs/`）

## 提交与代码规范

- 保持提交粒度小且聚焦单一目标
- C++ 代码遵循仓库 `.clang-format`（2 空格缩进、100 列）
- 推荐提交信息格式：`type(scope): subject`
  - type: `feat|fix|docs|refactor|test|chore`
  - subject 使用祈使句，简短明确

## Pull Request 要求

- PR 描述中说明「改了什么、为什么改、如何验证」
- 至少执行一次本地构建和测试并附结果
- 若涉及 DSL / 包规范 / API，请同步更新对应文档

## 问题反馈

- Bug 与功能建议请通过 GitHub Issues 提交
- 提交 Bug 时请提供复现步骤、期望结果和实际结果
