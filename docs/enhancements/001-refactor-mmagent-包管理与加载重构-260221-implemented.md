## MMAgent 包管理与加载重构 (2026-02-21)

### 功能概述
为了支持 MMAgent 的包管理与加载功能，进行了以下重构工作：
1. **支持目录方式加载 MMAgent**：
   - 在 `src/agent` 下新增 `package.h` 和 `package.cpp`，支持解析 `package.json` 和加载 MMAgent 包目录。
   - `package.json` 定义了 MMAgent 的名称、版本、入口点和工作流文件列表。
2. **简化 MMAgent 实例化**：
   - 在 `MMAgent` 类中新增 `create_from_package` 静态工厂方法，支持直接从目录创建 MMAgent 实例。
   - 更新了 `MMAgent` 类的成员函数，支持加载多个工作流。
3. **构建系统更新**：
   - 更新 `CMakeLists.txt` 链接 `dl` 库，支持动态加载。
   - 新增 `test_agent_loading` 测试目标。
4. **功能验证**：
   - 新增 `tests/functional/src/test_agent_loading.cpp` 功能测试，验证 MMAgent 包加载、生命周期管理和工作流执行。
   - 修复了 `AgentState` 命名空间引用的编译错误。
5. **示例增强**：
   - 在 `examples/agent` 目录下新增 `basic_agent` 和 `basic_agent` 示例包，包含完整的 `package.json` 和 `workflow.json`。

### 文件变更
- `include/internal/agent/package.h` (新增)
- `src/agent/package.cpp` (新增)
- `include/external/agent/agent.h` (修改)
- `src/agent/agent.cpp` (修改)
- `CMakeLists.txt` (修改)
- `tests/functional/src/test_agent_loading.cpp` (新增)
- `examples/agent/basic_agent/workflow.json` (新增)
- `examples/agent/basic_agent/workflow.json` (新增)

### 2026-06-25 Agent核心重构与事件系统合并

1. **运行时清理**：
   - 移除了未使用的 `src/runtime/network` 目录。
   - 删除了旧的 `src/runtime/event.cpp` 和 `include/internal/vendor/event/event.h`。
   - 删除了 `Testing` 目录及冗余的测试目录 (`tests/unit/workflow`, `tests/unit/runtime`, `tests/unit/agent`)。

2. **消息链路收敛**：
   - agent 侧不再保留独立事件机制，统一走 `msgque + databus` 协同链路。
   - 控制面消息仅承载路由与 `data_ref` 等元信息，payload 通过 MMDataBus 读取。
   - 相关 `test_event_gtest.cpp` 已移除，验证收敛到 agent/runtime 集成测试。

3. **MMAgent 核心组件增强**：
   - 实现了 `MMAgentSession` (会话管理), `MMAgentTask` (任务管理), `AgentMessages` (消息队列), `AgentContext` (上下文管理)。
   - `MMAgent` 类集成所有新组件，并在 `initialize_components` 中统一初始化。
   - 更新了 `src/agent/agent.cpp` 和 `include/external/agent/agent.h`。

4. **示例与目录重构**：
   - 将 `examples/dsl` 目录重构为 `examples/workflow`，包含原来的 DSL 示例和 JSON 工作流文件。
   - 将 `visualizer.html` 移至 `publish/agent` 目录。
   - 确保 `examples/agent` 包含 MMAgent 包示例 (`basic_agent`, `basic_agent`)。

### 文件变更
- `src/runtime/event.cpp` (删除)
- `include/internal/vendor/event/event.h` (删除)
- `src/agent/session.cpp` / `include/internal/agent/session.h` (新增/更新)
- `src/agent/task.cpp` / `include/internal/agent/task.h` (新增/更新)
- `src/agent/messages.cpp` / `include/internal/agent/messages.h` (新增/更新)
- `src/agent/context.cpp` / `include/internal/agent/context.h` (新增/更新)
- `src/agent/event.cpp` / `include/internal/agent/event.h` (删除)
- `src/agent/agent.cpp` (更新)
- `CMakeLists.txt` (更新)

### 2026-02-22 MMAgent 发布规范重构

1. **MMAgent 包规范落地**
   - 在 `publish/agent/AGENT_PACKAGE_SPEC.md` 新增 MMAgent 双层打包规范说明。
   - 在 `publish/agent` 补充 `agent.json`、`workflow_01.json`、`node_demo.sh` 模板。

2. **打包脚本升级**
   - `publish/agent_build.sh` 从 zip 产物调整为双层 `tar.gz` 产物：
     - `$xxxAgent_$PKGTYPE_$PKGVER.tar.gz`
     - `$xxxAgent_$AGTVER.tar.gz`

3. **开发文档重写**
   - `publish/README.md` 改为业务 MMAgent 包开发指南，覆盖 MMAgent/MMWorkflow/MMNode 开发流程。
   - 在 `publish/workflow` 新增 MMNode 规范与 API 手册：
     - `NODE_DEVELOPMENT_SPEC.md`
     - `NODE_API_REFERENCE.md`

4. **MMWorkflow 样例迁移**
   - `workflow_sample.jsonc` 迁移到 `publish/workflow/workflow_sample.jsonc`。
   - `src/workflow/dsl/README.md` 已更新引用路径。

5. **示例刷新**
   - 更新 `examples/agent/basic_agent` 与 `examples/agent/basic_agent`，补充规范化示例文件并保留现有加载兼容字段。

### 2026-03-07 MMWorkflow edge/node typed IO 重构

1. **MMEdge 控制模型收敛**
   - 删除 `loop/fork/join/async/goto` 控制语义，仅保留 `if` 与 `switch-case`。
   - `switch-case` 采用“多条 edge 共享 `expression`、每条 edge 自带 `case_value` / `is_default`”的分支模型。

2. **MMNode 输入输出类型化**
   - node 新增结构化 `inputs` / `outputs` 字段，支持 `string/int/float/bool/list/map`。
   - 每个字段支持 `transfer_mode: value|ref`，其中 `ref` 用于传递 DataBus 引用。

3. **样例/测试/发布资产同步**
   - 更新 `examples/`、`publish/workflow/pattern/`、`publish/workflow/workflow_sample.jsonc` 与 `tests/unit/src/test_workflow_json_gtest.cpp`。
   - 清理 `examples/agent/multi_session_chat` 下遗留的 `.bak` 备份文件。

### 2026-02-22 MMWorkflow MMEdge 控制与模式化重构

1. **MMEdge 控制统一化**
   - 扩展 MMEdge 配置，新增 `control.type + control.*` 机制，支持 `if/loop/fork/join/async/goto`。
   - DSL 解析器支持新控制字段，并保留 `condition_expression` 向 `if` 控制映射的兼容行为。

2. **MMWorkflow 流程能力增强**
   - 执行路径增加 edge 控制判断（含 async 软依赖、loop/goto 控制判定）。
   - 工作流图校验与拓扑排序对 `loop/goto` 控制边放宽，支持显式环回流程。

3. **模式资产新增**
   - 新增 `publish/workflow/pattern/serial_workflow.json`。
   - 新增 `publish/workflow/pattern/parallel_workflow.json`。
   - 新增 `publish/workflow/pattern/cycled_workflow.json`。

4. **MMNode 模式模板新增**
   - `publish/workflow/node_patterns/` 新增问答/API工具/MCP/skills 四类模板文档。

5. **文档同步**
   - 更新 `publish/README.md`、`publish/workflow/README.md`、`publish/workflow/workflow_sample.jsonc`、`src/workflow/dsl/README.md`。

6. **测试增强**
   - `tests/unit/src/test_workflow_json_gtest.cpp` 增加 edge control 解析与 goto 环回流程解析测试。

### 2026-02-22 MMWorkflow 动态生成与嵌套重构

1. **MMNode/MMWorkflow Skill 化**
   - 新增 `src/workflow/node/skill.md`，定义 node 能力说明、调用方法、输入输出契约。
   - 新增 `src/workflow/dsl/skill.md`，定义 workflow 编排技能、include 调用方式与控制约定。

2. **嵌套 MMWorkflow 支持**
   - 扩展 `MMNode::Config`，新增 `workflow_ref` 字段，支持 `type: "workflow"` 节点引用子工作流。
   - `WorkflowParser` 支持顶层 `includes`（相对路径）并合并子工作流节点/边。
   - 增加 include 循环引用检测，防止递归 include 造成死循环。

3. **动态流程模式新增**
   - 新增 `publish/workflow/pattern/dynamic_workflow.json`：
     - `start -> selector -> (dynamic node or dynamic workflow) -> end`
     - 使用 `goto` 控制边表达动态跳转。

4. **文档与测试同步**
   - 更新 `publish/workflow/README.md`、`publish/README.md`、`src/workflow/dsl/README.md`。
   - 增强 `tests/unit/src/test_workflow_json_gtest.cpp`：覆盖 include 解析、workflow 类型节点解析、dynamic pattern 文件解析。

5. **验证结果**
   - `./build.sh build` 通过。
   - `./build.sh test` 并行存在热重载波动；串行兜底 `ctest --test-dir build --output-on-failure -j 1` 全量通过。

### 2026-02-22 MMWorkflow 重构后兼容性巡检修复

1. **Visualizer 新 DSL 兼容修复**
   - 修复 `publish/agent/visualizer.html` 对新 workflow 字段的兼容：
     - 支持 `.json/.jsonc` 读取（轻量 JSONC 去注释）。
     - 支持 edge `control.type + control.*` 展示与边样式区分。
     - 支持 node `workflow_ref` 和 workflow `includes` 元信息展示。

2. **有环流程可视化修复**
   - 解决旧工作流图入度布局在 `goto/loop` 环回流程上的坐标缺失问题。
   - 增加有环图 fallback 层级计算，保证节点可渲染。

3. **文档同步**
   - 更新 `publish/workflow/README.md` 与 `publish/README.md`，补充 visualizer 对新 DSL 的支持说明。

4. **验证**
   - 执行构建与测试回归，确认修复不影响主流程。
