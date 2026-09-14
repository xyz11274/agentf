# Harness 图集

本目录保存 `harness-deep-dive/README.md` 使用的可编辑 SVG 源文件。

| 文件 | 视角 | 适合回答的问题 |
|---|---|---|
| `01-system-landscape.svg` | 系统上下文 / 分层架构 | Harness 与 AgentFlow、Bridge、Sandbox 的边界是什么？ |
| `02-harness-anatomy.svg` | Harness 内部组件 | 一轮 LLM 到底有哪些支撑模块参与？ |
| `03-dual-mode-architecture.svg` | v1/v2 对比 | 两种 Coordinator 模式的控制权和代价有何不同？ |
| `04-v2-request-sequence.svg` | 默认运行时序 | 请求、工具调用、流式进度和最终响应如何流转？ |
| `05-package-lifecycle.svg` | 资产与部署 | Template、Stored Package、Workspace、Sandbox 如何派生？ |

## 视觉约定

- 青色实线：控制或调用；
- 黄色虚线：结果、状态或 DataBus 数据；
- 紫色点线：资产装配、配置或流式侧链；
- 蓝色区域：AgentFlow/C++ 执行层；
- 紫色区域：Harness/LLM 认知层；
- 绿色区域：Tool Executor 与隔离执行环境。

SVG 使用固定 `viewBox`、内嵌样式和系统字体栈，不依赖外部图片、字体或脚本，可直接在浏览器、IDE Markdown 预览和文档站点中显示。
