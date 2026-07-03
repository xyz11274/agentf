# Bridge 测试文档

| 文档 | 用途 |
|------|------|
| [E2E 测试设计](e2e-test-design.md) | Bridge → daemon → Agent → Artifact 完整链路的约束、风险与测试方案 |
| [测试目录](test-catalog.md) | TC ID、约束、自动化位置和实施状态的全局索引 |

测试实现后，必须同步更新测试目录中的“自动化位置”和“状态”，并在 pytest 用例 docstring 首行写入相同 TC ID。
