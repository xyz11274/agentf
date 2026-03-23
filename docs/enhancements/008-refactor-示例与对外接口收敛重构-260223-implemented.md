## 示例与对外接口收敛重构（2026-02-23）

- 示例体系收敛：
  - `examples/p1_features_example.cpp` 重构为 `examples/databus_example.cpp`，仅保留 databus 用法；
  - 删除 `examples/basic_example.cpp`、`examples/workflow/{dsl_example,node_execution_example}.cpp`；
  - 删除 `examples/agent/{agent_example,agent_loader_example}.cpp`；
  - 将 `examples/workflow/*.json` 合并到 `examples/agent/`，统一为 agent 包侧示例资产。
- 构建脚本与目标调整：
  - `CMakeLists.txt` 移除 basic/workflow/agent 代码示例目标，新增 `example_databus`；
  - `build.sh example` 默认示例调整为 `databus`。
- 对外头文件发布面收敛：
  - `include/external` 仅保留 node 定制开发接口；
  - 删除 `include/external/agent/agent.h` 与 `include/external/workflow/{workflow.h,edge.h,control_flow.h}`；
  - `include/internal/agent/agent.h` 承接 MMAgent 声明能力，保证内部编译路径稳定。
- 文档与测试：
  - `examples/README.md` 改为 agent 包视角说明，补充 `AgentService.config`、自动加载与请求处理链路说明；
  - `tests/functional/src/test_agent_loading.cpp` 新增 MMAgentActor + MMRuntimeActor 自动加载/注册链路测试（RapidJSON 开启时执行）。
