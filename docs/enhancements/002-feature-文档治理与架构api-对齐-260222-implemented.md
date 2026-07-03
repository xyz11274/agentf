## 文档治理与架构/API 对齐（2026-02-22）

- 删除重复文档：`docs/contributing.md`，并将保留的贡献规范收敛到根目录 `CONTRIBUTING.md`。
- 修正全仓链接：将原 `docs/CONTRIBUTING.md` 引用统一到 `CONTRIBUTING.md`，避免失效链接。
- 重写 `docs/ARCHITECTURE.md`，按当前实现统一为：
  - Framework 两层：MMAgent + MMWorkflow（MMNode/MMEdge 为 MMWorkflow 组成）
  - Runtime 两类调度：`task -> workflow instance` 与 `workflow instance -> worker`
  - 共享基础设施：MMDataBus
- 重组 `docs/api-reference.md`，按 Framework / Runtime / MMDataBus 三层组织接口说明，并补充 MMAgent 包、MMWorkflow DSL 与 MMNode 开发 API 的索引关系。
- 构建与测试回归：
  - `./build.sh build` 通过；
  - `./build.sh test` 出现既有 HotReload 并行抖动；
  - `ctest --test-dir build --output-on-failure -j 1` 串行回归 10/10 通过。
