# 项目目录结构优化说明

本文档说明了 AgentFlow 项目目录结构的优化工作。

## Agent 自主能力增强（2026-02-23）

- `src/agent/agent_actor.cpp` 重构为自动化实例管理能力：
  - 从 `AgentService.config`（JSON）读取 `package_root` 与 `manifests`；
  - 自动加载 agent 包并创建 Agent 实例；
  - 启动包监控线程，目录变化时增量加载新版本实例，支持多版本并行运行；
  - 维护 agent/task 实例清单，并提供 `agent.start/pause/resume/stop`、`task.start/pause/stop` 控制消息入口。
- 按要求将请求事件处理能力下沉到 `Agent`：
  - `include/external/agent/agent.h`、`src/agent/agent.cpp` 新增请求事件队列与监控线程；
  - 新请求入队后触发 Agent 内 `AgentCoordinator` 对默认会话及其他会话收发队列进行扫描处理，直到队列清空；
  - 处理生成的 task 通过 Agent 接口回传给 AgentActor 统一管理与上报。
- 新增独立 `RuntimeActor`：
  - 新增 `include/internal/runtime/runtime_actor.h`；
  - `src/runtime/runtime_actor.cpp` 新增 RuntimeActor 消息处理，接收 agent/task/worker 的注册、心跳、状态上报并对接 `AgentRuntime`。
- Worker 目录与 actor 化能力增强：
  - `src/runtime/worker.cpp` 迁移为 `src/runtime/worker/worker.cpp`；
  - 新增 `include/internal/runtime/worker/worker_actor.h` 与 `src/runtime/worker/worker_actor.cpp`；
  - WorkerActor 负责 worker 注册、心跳、状态同步消息上报到 RuntimeActor。
- 构建与测试验证：
  - `./build.sh build` 通过；
  - `./build.sh test` 通过（10/10）。

## Node 执行引擎下沉 + Coordinator Task0 默认化（2026-02-28）

- Node 多语言执行引擎下沉到 Worker：
  - `include/internal/runtime/worker.h` 增加 Worker 执行请求/结果模型与 `execute_code` 入口；
  - `src/runtime/worker/worker.cpp` 实现 shell/python/go/jar/so/executable 分发执行；
  - `src/workflow/node/node.cpp` 的外部执行模式统一改为委托 worker 执行，Node 保留状态机与输入输出管理。
- Coordinator 默认 task 语义统一为 task0：
  - `include/internal/agent/coordinator.h` / `src/agent/coordinator.cpp` 增加默认 task 类型配置，缺省改为 `task0`；
  - 新增 task0 workflow 解析逻辑：优先使用 `coordinator_workflow`，未配置时兼容回退 `task_workflows["0"]`，支持按 workflow id 或 name 匹配。
- Session/Coordinator 语义对齐：
  - 默认会话统一为 `0` 号 `AgentSession`；
  - 每个 session 创建时默认注入 `task0`（`<session_id>_task_0`）并创建该 session 专属 coordinator；
  - session 消息处理改为使用 session 级 coordinator，确保 coordinator 归属 session。
- Agent 包新增 task0 workflow 定制能力：
  - `include/internal/agent/package.h` / `src/agent/package.cpp` 新增 `coordinator_workflow` 配置解析；
  - `src/agent/agent.cpp` 初始化 coordinator 时注入包配置中的固定 workflow；
  - `publish/agent/package.json` 与 examples 增加 `coordinator_workflow.json`，用于演示 session 创建节点与 task 创建节点编排。
- 测试增强：
  - `tests/unit/src/test_agent_gtest.cpp` 新增 task0 默认行为和 package task0 workflow 映射测试；
  - `tests/unit/src/test_builtin_workflow_gtest.cpp` 增加 `coordinator_workflow` 解析断言；
  - `tests/unit/src/test_runtime_master_worker_gtest.cpp` 新增 worker 执行引擎 shell 路径测试。

## Node 热更新能力下沉到 Worker（2026-02-28）

- 热更新控制面迁移到 Worker：
  - `include/internal/runtime/worker.h` 新增 `check_and_reload_code/get_code_version/rollback_code_version/stop_hot_reload`；
  - `src/runtime/worker/worker.cpp` 统一执行与热更新能力，支持运行中版本更新与回退。
- Node 热更新 API 保持兼容但改为 Worker 委托：
  - `src/workflow/node/node.cpp` 中 `reload/check/version/rollback` 统一转发到 Worker；
  - 外部执行请求携带 `enable_hot_reload/hot_reload_config`，默认配置无需额外 `enable_code_reload` 调用。
- 代码归属调整：
  - `src/workflow/node/exe_loader.cpp` 迁移到 `src/runtime/worker/exe_loader.cpp`，热更新实现与 Worker 执行引擎同域管理。
- 测试补充：
  - `tests/unit/src/test_runtime_master_worker_gtest.cpp` 新增 Worker 热更新+回退用例；
  - `tests/unit/src/test_hot_reload.cpp` 刷新 Node 场景为默认热更新语义。

## Runtime Master 自主调度重构（2026-02-23）

- `src/runtime/runtime.cpp` 增强全局 Agent 管理能力：
  - 新增 Agent 注册请求队列与注册处理线程（`agent_registration_loop`）；
  - 新增 Agent 心跳上报与监控线程（`report_agent_heartbeat` / `agent_monitor_loop`）；
  - Agent 故障时联动 Deployer/Scheduler 执行 workflow 注销与 task 映射刷新。
- `include/internal/runtime/shared_runtime_state.h` 扩展共享状态：
  - 增加 `TaskInstanceInfo`；
  - 增加 workflow 按 agent/workflow 检索接口；
  - 增加 task 实例增删改查接口，支撑调度状态追踪。
- `src/runtime/master/deployer.cpp` / `include/internal/runtime/master/deployer.h` 重构：
  - 增加 worker 注册队列与处理线程、部署调度线程、心跳超时故障隔离；
  - 支持 workflow 增量部署与 `rebalance`；
  - 增加 so 可插拔部署算法加载（失败自动回退内置算法）。
- `src/runtime/master/scheduler.cpp` / `include/internal/runtime/master/scheduler.h` 重构：
  - 增加 task 注册队列与调度线程、task->workflow 映射缓存和生命周期状态更新；
  - 默认调度策略优先同 agent instance，再同 agent object，最后全局最小负载；
  - 增加 so 可插拔调度算法加载（失败自动回退内置算法）。
- 验证结果：
  - `./build.sh build` 通过；
  - `./build.sh test` 受既有 HotReload 并行抖动影响失败；
  - `ctest --test-dir build --output-on-failure -j 1` 串行回归 10/10 通过。

## Visualizer GOTO 环路可视化修复（2026-02-23）

- 优化 `publish/agent/visualizer.html` 的边绘制逻辑，新增对回跳边（`goto` 或目标层级在左侧）的专用贝塞尔路径路由：
  - 回跳边从源节点左侧出发，目标节点右侧入边，并抬升弧线高度，避免与前向边重叠；
  - 解决 `publish/workflow/pattern/cycled_workflow.json` 中 `evaluate -> decision` 的 `goto` 控制边不可辨识问题。
- 标签与箭头计算改为基于贝塞尔曲线采样点：
  - 箭头方向使用曲线末端切线，保证回跳边箭头方向正确；
  - 控制类型标签（如 `GOTO`）在曲线上居中显示，回跳边额外上移，提升可读性。
- 边命中检测改为复用统一曲线路径计算，保证回跳边可点击选中并查看属性。
- `fitView` 增加视图留白，避免环路弧线被视口裁切。

## Worker/Workflow/Node/Edge 自主执行重构（2026-02-23）

- `runtime/worker` 增强为自驱动执行模型：
  - 新增高优先级/普通任务队列、workflow 实例登记队列、事件队列；
  - 新增事件监控线程、事件处理线程池和 workflow 处理线程池；
  - 新 task 入队会触发事件并优先调度高优任务；
  - 新 workflow 实例登记时会按配置动态创建对应处理线程池。
- `workflow/workflow` 增强运行时能力：
  - 新增 workflow 级高优先级/普通消息队列与事件队列；
  - 新增消息调度线程、事件调度线程、消息处理线程池和 node 执行线程池；
  - 消息执行采用主控调度逻辑，按 edge 依赖进行分层串并执行，并推送完成/失败事件。
- `workflow/node` 增强运行时能力：
  - 新增 node 级消息队列、事件队列、executor 线程池和事件处理线程池；
  - 支持消息驱动执行与事件处理器注册。
- `workflow/edge` 增强运行时能力：
  - 新增 edge 级消息队列、事件队列、processor 线程池和事件处理线程池；
  - 支持消息驱动的 edge 处理与事件处理器注册。
- 线程/并发配置入口统一放在 `runtime::AgentRuntime::Config`，新增：
  - `worker_default_workflow_threads`
  - `worker_event_threads`
  - `worker_queue_size`
- 验证结果：
  - `./build.sh build` 通过；
  - `./build.sh test` 仍存在既有 HotReload 并行抖动；
  - `ctest --test-dir build --output-on-failure -j 1` 串行回归 10/10 通过。

## Vendor 依赖配置与文档刷新（2026-02-23）

- `vendor/vendor.conf` 补充两项依赖配置：
  - `Protoactor-cpp|https://github.com/DanielKernel/Protoactor-cpp.git|main|Protoactor-cpp`
  - `RapidJSON|https://github.com/Tencent/rapidjson.git|master|rapidjson`
- `vendor/README.md` 已刷新并中文化，统一说明目录结构、配置格式、`vendor.sh` 用法和适配器依赖映射关系。
- 构建与测试验证：
  - `./build.sh build` 通过；
  - `./build.sh test` 出现既有 HotReload 并行抖动；
  - `ctest --test-dir build --output-on-failure -j 1` 串行回归 10/10 通过。

## 文档治理与架构/API 对齐（2026-02-22）

- 删除重复文档：`docs/contributing.md`，并将保留的贡献规范收敛到根目录 `CONTRIBUTING.md`。
- 修正全仓链接：将原 `docs/CONTRIBUTING.md` 引用统一到 `CONTRIBUTING.md`，避免失效链接。
- 重写 `docs/ARCHITECTURE.md`，按当前实现统一为：
  - Framework 两层：Agent + Workflow（Node/Edge 为 Workflow 组成）
  - Runtime 两类调度：`task -> workflow instance` 与 `workflow instance -> worker`
  - 共享基础设施：DataBus
- 重组 `docs/api-reference.md`，按 Framework / Runtime / DataBus 三层组织接口说明，并补充 Agent 包、Workflow DSL 与 Node 开发 API 的索引关系。
- 构建与测试回归：
  - `./build.sh build` 通过；
  - `./build.sh test` 出现既有 HotReload 并行抖动；
  - `ctest --test-dir build --output-on-failure -j 1` 串行回归 10/10 通过。

## DataBus 全局共享重构（2026-02-22）

- DataBus 增强为“本地缓存优先 + 远端服务同步”模型：支持进程内、跨进程、跨节点共享链路。
- 新增 DataRef 流程：`store_with_ref / resolve_ref / find_refs_by_owner`。
- owner 维度索引支持：`request_id/user_id/client_id/agent_id/session_id/task_id/workflow_id/node_id/master_id/worker_id`。
- `msgque/event` 协同增强：控制面仅携带 `data_ref`，payload 由 DataBus 本地缓存读取，本地 miss 自动回源并回填。
- Redis 依赖接入：`vendor/vendor.conf` 增加 redis 仓库条目，新增 `vendor/adapter/cache/redis_adapter.cpp` 适配实现。
- 回归结果：
  - `./build.sh build` 通过；
  - `./build.sh test` 存在既有 HotReload 并行抖动；
  - `ctest --test-dir build --output-on-failure -j 1` 串行 10/10 通过。

## 历史 TODO 合并记录（2026-02-22）

- 原根目录 `TODO.md` 已合并并下线，规划内容统一收敛到本文件与 `docs/README.md` 导航。
- 历史状态快照（来自旧 TODO）：
  - 开发编排框架：约 85%
  - 运行框架：约 88%
  - 监控和日志：约 68%
- 已完成主线（P0/P1）：AgentRuntime 调度闭环、C++ 节点执行、循环控制、序列化链路、DataBus 通知等。
- 后续主线（P2）：性能与稳定性增强（共享内存、worker 分组、热更新稳定性、测试抖动治理）。

## 目录结构总览

```
AgentFlow/
├── CMakeLists.txt        # 顶层构建入口
├── build.sh              # 构建入口脚本
├── build/                # 构建输出目录和CMake配置
│   ├── cmake/           # CMake 配置文件
│   └── install_dependencies.sh
├── include/             # 头文件目录
│   ├── external/        # 公共 API 头文件（随库发布）
│   └── internal/        # 内部实现头文件（仅供内部使用）
├── src/                 # 核心业务源代码
├── vendor/              # 三方依赖和适配器
│   ├── vendor.conf     # 三方依赖配置文件
│   ├── vendor.sh       # 三方依赖下载脚本
│   ├── README.md       # 使用文档
│   └── adapter/        # 三方依赖适配器实现
├── tests/               # 测试目录
│   ├── unit/           # 单元测试
│   └── functional/     # 功能测试
├── examples/           # 示例代码
├── docs/               # 项目文档
```

## 优化内容

### 0. Agent 消息链路与会话协同增强（2026-02-21）

本次优化将 Agent 内部消息处理从单消息容器升级为会话化消息队列协同模型，主链路为：

`reqmsg -> agent.defaultsession -> agent.newsession -> agent.coordinator -> agent.newsession.task -> agent.workflow`

核心变更：
- `src/agent/messages.cpp` 迁移为 `src/agent/msgque.cpp`，核心类升级为 `AgentMsgQue`（保留 `AgentMessages` 兼容别名）。
- 每个 Agent 实例持有默认会话 `default_session`，请求消息先进入默认会话接收队列。
- 会话按 `user_id + client_id` 动态创建，子会话持有独立接收/发送队列与任务集合。
- 新增 `AgentCoordinator`（`src/agent/coordinator.cpp`），统一处理会话消息并生成任务：
  - 执行任务：映射到某个 workflow 类型；
  - 协同任务：支持通过 so 路径指定协同算法扩展。
- 任务归属模型调整：task 必须绑定其触发会话，并保留 workflow 映射信息供 runtime scheduler 消费。

### 0.1 Runtime 全局 Master/Worker 能力增强（2026-02-21）

本次补齐 Runtime 的全局部署与调度能力，支持 master/worker 角色化运行：

- 新增 `runtime/worker`：Worker 作为 workflow/node 执行引擎抽象，维护负载、状态、心跳与支持的 workflow 类型集合。
- 新增 `runtime/master/deployer`：负责全局 worker 注册、心跳/状态更新、workflow 实例到 worker 的部署映射、映射刷新与版本更新。
- 新增 `runtime/master/scheduler`：负责全局 task 调度到 workflow 实例，再间接落到对应 worker。
  - 调度策略：优先同 agent 实例，再回退到同 agent 对象的其他实例共享 workflow。
- 新增 `runtime/shared_runtime_state` 抽象：本轮实现“接口 + 本地内存默认实现”，用于多实例共享语义与 workflow 实例引用计数。
- `AgentRuntime` 增强：
  - 支持 role 配置（`MASTER` 包含 `WORKER` 能力，`WORKER` 不包含 master 能力）；
  - 暴露 worker 注册/心跳/状态接口；
  - 暴露 workflow 实例部署与销毁、task 调度接口，形成 task -> workflow instance -> worker 闭环。

### 0.2 Workflow 内建模板能力增强（2026-02-21）

新增内建 workflow 模板目录，支持开箱即用与业务定制增强：

- `publish/workflow/memory/`
- `publish/workflow/skills/`
- `publish/workflow/tools/`

每个目录均包含默认 `workflow.json` 与 `nodes/` 节点脚本，可在业务侧直接替换节点脚本并扩展流程图。

打包策略采用按需选择（opt-in）：
- Agent `package.json` 可配置 `builtin_workflows`（如 `["memory","skills","tools"]`）。
- 构建脚本 `publish/agent_build.sh` 在打包时仅注入声明的内建 workflow 到包内 `workflows/builtin/<name>/`。
- AgentPackage 加载时支持解析 `builtin_workflows` 并将可解析的内建流程文件加入 workflow 列表。

### 0.3 今日优化交付与验证快照（2026-02-21）

本轮代码已完成“Agent 消息链路 + Runtime 全局调度 + Workflow 内建模板”三条主线的端到端打通，关键更新如下：

- **Agent 路径闭环**：`reqmsg -> default_session -> dynamic_session -> coordinator -> session_task -> workflow` 已落到代码与测试。
- **会话与任务归属强化**：每个 Agent 固定默认会话，动态会话按 `user_id + client_id` 拆分；Task 仅归属触发会话并携带 workflow 映射元数据。
- **Runtime 主从能力成型**：`MASTER` 包含 `WORKER` 能力，新增 deployer/scheduler/worker/shared state 协作链路，支持 worker 注册、心跳、负载调度与 workflow 实例映射刷新。
- **内建 workflow 资产化**：新增 `publish/workflow/{memory,skills,tools}`，支持 `builtin_workflows` 按需打包、解析和加载，并保留业务定制入口。
- **测试与构建验证**：最新全量回归通过（`ctest --output-on-failure`，10/10 passed）。

### 0.4 下一步继续优化方向（基于当前代码状态）

1. **共享状态后端外置化**：将 `IRuntimeSharedState` 从内存实现扩展到可插拔外部存储（如 Redis/etcd），支撑多 runtime 实例真实共享。
2. **deployer 调度策略增强**：在状态/负载基础上补充容量阈值、故障隔离与预热策略，提升高并发下的稳定性。
3. **scheduler 可观测性与可解释性**：补充 task->workflow->worker 的调度决策日志与指标，支持按会话和 agent 维度追踪。
4. **内建 workflow 版本化管理**：为 memory/skills/tools 引入版本字段与兼容策略，支持灰度升级与回滚。
5. **协同任务插件治理**：对 so 协同算法增加加载约束、校验与隔离机制，降低线上扩展风险。

### 1. 头文件目录重组

**目录结构：**
```
include/
├── external/           # 公共 API 头文件（供外部调用方使用）
│   ├── agent/          # agent实例化：提供agent实例生命周期管理接口
│   │   ├── agent.h
│   │   └── common.h
│   └── node/           # node定制开发：基于node控制面/运行面接口开发定制node
│       ├── node.h
│       ├── hot_reload.h
│       └── node/
│           └── code_loader.h
└── internal/           # 内部实现头文件（仅供内部系统使用）
    ├── agent/          # agent实现，单agent实例、多会话、多消息、多workflow任务
    ├── workflow/       # workflow实现，包含node和edge
    ├── runtime/        # master/worker部署，多agent、多workflow、多worker调度
    ├── vendor/         # 三方依赖适配
    │   ├── json_adapter/
    │   └── private_protocol.h
    ├── serialization/
    ├── actor_adapter/  # Actor 适配器实现
    └── testing/        # 测试工具
```

**说明：**
- `external/` 仅暴露 `agent/` 与 `node/` 最小外部接口
- `internal/` 承载 `runtime`、`storage`、`workflow` 等全部内部实现能力

### 2. 测试目录重组

**目录结构：**
```
tests/
├── unit/               # 单元测试
│   ├── agent/          # C++ 单元测试代码（当前预留空目录）
│   ├── workflow/       # 测试数据/脚本/测试库与单元测试代码（当前承载既有 unit 内容）
│   │   ├── cpp/
│   │   ├── data/
│   │   ├── jars/
│   │   ├── lib/
│   │   └── scripts/
│   └── runtime/        # 测试库（如 hot_reload_test，当前预留空目录）
└── functional/         # 功能测试
    ├── cpp/            # 功能测试代码（test_performance.cpp 等）
    └── scripts/        # 功能测试脚本
```

**说明：**
- `unit/agent` 与 `unit/runtime` 当前为预留目录
- 现有 unit 测试内容按约定统一落在 `unit/workflow/`（含 cpp/data/lib/scripts/jars）
- `functional/` - 功能测试，包括性能测试、集成测试等

### 3. 三方依赖与适配器目录

**目录结构：**
```
src/                    # 业务代码相关实现，和三方实现独立目录
vendor/                 # 三方依赖相关实现
├── vendor/vendor.conf  # 三方依赖配置文件
├── vendor.sh           # 三方依赖下载脚本
├── README.md           # 使用文档
├── adapter/            # 三方依赖适配器实现
│   ├── actor/          # Actor 框架适配器
│   │   ├── protoactor_impl.cpp
│   │   └── stub_impl.cpp
│   ├── dsl/            # DSL 解析器适配器
│   │   ├── json_adapter_rapidjson.cpp
│   │   └── json_adapter_stub.cpp
│   └── serialization/  # 序列化适配器（adapter_factory.cpp）
└── <downloaded>/       # 下载的三方依赖（gitignore）
```

**说明：**
- `src/` - 保留核心业务代码，不包含适配器实现
- `vendor/` - 所有三方依赖相关内容统一放置
- `vendor/vendor.conf` - 三方依赖下载配置
- `vendor/adapter/` - 与三方依赖相关的适配器代码

### 4. 源代码目录

**目录结构：**
```
src/
├── agent/              # Agent 实例管理实现（agent.cpp）
├── workflow/           # Workflow 管理实现
│   ├── dsl/            # DSL 解析实现
│   ├── edge/           # Edge 子模块实现
│   ├── node/           # Node 子模块实现（code_loader, node_actor 等）
│   ├── workflow.cpp    # Workflow 核心实现
│   ├── node/
│   │   └── node.cpp    # Node 核心实现
│   └── edge/
│       └── edge.cpp    # Edge 核心实现
├── runtime/            # 运行时实现
│   ├── scheduler/      # 调度器（agent_runtime, control_flow）
│   ├── communication/  # 通信（databus, hot_reload, local_channel, zero_copy_databus）
│   ├── storage/        # 存储（agent_storage, storage_manager）
│   └── actor/          # Actor 系统运行时（actor_system, node_actor 等）
├── vendor/             # 三方适配实现（actor/dsl/event/serialization/storage）
└── utils/              # 工具实现（logger, thread_pool）
```

**说明：**
- `agent/`：实现以下模块：1、agent实例管理模块：支持agent生命周期及状态管理，比如创建（参考agent包里面包含的workflow流程文件和node二进制实现进行实例化）->CREATED，agent实例运行->RUNNING、暂停->PAUSED、停止->STOPPED；2、Agent实例对应会话管理，创建、销毁会话等；3、Agent收发消息管理：agent实例接收的消息缓存、映射到会话及任务；4、Agent任务管理：创建执行任务实例，任务实例与workflow实例映射，Agent只做任务的状态管理，任务到workflow实例需要等待调度
- `workflow/`：实现以下模块：1、workflow管理模块：比如创建workflow实例（低码化DSL、高码化C++接口）；2、node实现（code_loader、node_actor等）；3、edge实现；4、DSL解析
- `runtime/`：实现调度和执行：1、scheduler/：master将workflow到worker调度，所有agent实例生成的workflow统一调度到worker上；control_flow控制流；2、communication/：databus高速数据共享、hot_reload热重载、local_channel本地通道；3、storage/：agent和workflow持久化；4、actor/：运行时Actor系统
- `vendor/` 目录已移出 `src/`，三方依赖适配器统一在 `vendor/adapter/` 中管理

### 5. 构建系统

**构建入口：**
- `build.sh` - 项目根目录的构建入口脚本

**构建输出：**
- `build/` - 构建输出目录，包含编译产物和 CMake 配置

## 使用说明

### 构建项目

```bash
# 构建项目
./build.sh build

# 清理构建
./build.sh clean

# 重新构建
./build.sh rebuild

# 运行测试
./build.sh test

# 运行示例
./build.sh example basic
```

### 下载三方依赖

```bash
# 下载所有配置的三方依赖
./vendor/vendor.sh

# 查看已配置的依赖
./vendor/vendor.sh --list

# 更新已下载的依赖
./vendor/vendor.sh --update

# 清理并重新下载
./vendor/vendor.sh --clean
```

### 安装构建依赖

```bash
# 安装系统依赖和开发环境
./build/install_dependencies.sh
```

### 运行测试

```bash
# 运行单元测试
./build.sh test

# 运行功能测试
./tests/functional/scripts/run_performance_tests.sh
```

## CMake 配置说明

### Include 目录配置

```cmake
# 公共 API（用于外部用户）
include_directories(${CMAKE_SOURCE_DIR}/include/external)

# 内部实现（仅用于源代码编译）
include_directories(${CMAKE_SOURCE_DIR}/include/internal)
```

### 源文件组织

```cmake
# 核心业务代码
set(AGENTFLOW_SOURCES
    ${AGENTFLOW_SOURCE_DIR}/agent/...
    ${AGENTFLOW_SOURCE_DIR}/workflow/...
    ${AGENTFLOW_SOURCE_DIR}/runtime/...
    ${AGENTFLOW_SOURCE_DIR}/vendor/...
    ${AGENTFLOW_SOURCE_DIR}/utils/...
)

# 适配器代码（三方依赖，条件编译）
set(AGENTFLOW_ADAPTER_SOURCES
    ${AGENTFLOW_VENDOR_DIR}/adapter/actor/...
    ${AGENTFLOW_VENDOR_DIR}/adapter/dsl/...
    ${AGENTFLOW_VENDOR_DIR}/adapter/event/...
    ${AGENTFLOW_VENDOR_DIR}/adapter/network/...
    ${AGENTFLOW_VENDOR_DIR}/adapter/serialization/...
)
```

## 设计原则

1. **关注点分离**：
   - `include/external/agent` + `include/external/workflow` - 公共 API，面向外部用户
   - `include/internal/` - 内部实现，面向开发者
   - `src/` - 核心业务逻辑
   - `vendor/` - 三方依赖适配

2. **模块化**：
   - 适配器与核心业务分离
   - 单元测试与功能测试分离

3. **清晰的边界**：
   - 外部用户只能使用 `external/agent` 与 `external/workflow` 接口
   - 内部实现细节对用户透明

---

**更新日期**：2026 年 2 月 19 日


---

## 合并内容：原 planning.md

# AgentFlow 项目规划

> 本文档整合了 AgentFlow 项目的功能实现状态、开发规划、优先级和代码完备性评估。

**最后更新**: 2026年2月18日

---

## 目录

1. [功能实现状态概览](#功能实现状态概览)
2. [已完成功能详情](#已完成功能详情)
3. [待实现功能（按优先级）](#待实现功能按优先级)
4. [近期开发重点](#近期开发重点)
5. [版本规划](#版本规划)
6. [代码完备性评估](#代码完备性评估)
7. [代码质量目标](#代码质量目标)
8. [文档目标](#文档目标)
9. [构建和部署目标](#构建和部署目标)
10. [更新日志](#更新日志)

---

## 功能实现状态概览

| 功能模块 | 完成度 | 状态说明 |
|---------|--------|---------|
| 开发编排框架 | 85% | Agent、Workflow、Node、Edge 定义完成，存储功能已实现 |
| 运行框架 | **88%** | 核心调度完成，控制流完成，P0/P1 缺陷已修复 |
| 监控和日志 | **68%** | 基础监控完成，跨平台实现完成 |

**进度提升**:
- 运行框架：75% -> 88% (+13%)
- 基础设施：48% -> 58% (+10%)
- 监控和日志：58% -> 68% (+10%)

---

## 已完成功能详情

### 第一阶段：开发编排框架（85% 完成）

#### Agent 编排
- 已完成 Agent 对象定义
- 已完成 Agent 生命周期策略（PER_REQUEST、FACTORY、SINGLETON）
- 已完成 Agent 版本管理基础字段
- 待完成 Agent 资源访问权限管理
- 待完成 Agent 版本管理逻辑（版本比较、兼容性检查）
- 待完成 Agent 灰度发布配置逻辑

#### Workflow 编排
- 已完成 完整的 DSL 解析实现（WorkflowParser）
- 已完成 DSL 验证功能
- 已完成 Workflow 版本管理基础字段
- 已完成 灰度发布支持
- 已完成 版本回滚机制
- 待完成 Workflow 版本管理逻辑（版本比较、兼容性检查）

#### Node 实现
- 已完成 Node 控制面结构
- 已完成 Node 输入输出接口定义
- 已完成 Node 错误处理策略（重试、超时）
- 已完成 Node 资源限制
- 已完成 控制面/运行面分离架构
- 已完成 多语言 Node 支持（Shell、Python、SO、JAR）
- 已完成 C++ 函数执行（cpp_function 回调 + CodeLoader 回退）
- 已完成 运行面 DSL 配置

#### Edge 定义
- 已完成 依赖关系定义
- 已完成 数据类型定义
- 已完成 数据传递方式（DataTransferMode）
- 已完成 控制流事件定义
- 已完成 Edge 条件表达式支持
- 已完成 Edge 条件评估实现

#### 持久化
- 已完成 本地存储 Agent 定义
- 已完成 本地存储 Workflow 定义
- 已完成 版本管理和变更历史
- 已完成 灰度发布配置存储
- 待完成 CI/CD 流程集成接口
- 待完成 远程存储支持（S3、HDFS）

---

### 第二阶段：运行框架（88% 完成）

#### Agent 运行管理
- 已完成 Agent 实例定义
- 已完成 Agent 生命周期和状态管理
- 已完成 Agent 内部资源管理
- 已完成 Agent 实例策略
- 已完成 微服务层接口封装（内部函数）
- 待完成 Agent 对外统一访问接口（依赖微服务框架）
- 待完成 多种协议支持（依赖微服务框架）

#### Workflow 运行和 Node 调度
- 已完成 DSL 动态加载和解析
- 已完成 Workflow 实例创建
- 已完成 Workflow 动态加载、卸载和版本更新接口
- 已完成工作流图调度算法（Kahn 算法拓扑排序）
- 已完成 并行执行互不依赖的 Node
- 已完成 Node 执行状态管理
- 已完成 Node 输入输出数据处理
- 已完成 Node 生命周期管理
- 已完成 复杂流程控制（条件、循环、异常处理）
  - FOR_EACH 循环实现
  - 条件表达式解析缓存
- 已完成 复杂流程控制与 Workflow 调度集成
  - on_node_complete 调度闭环修复
- 待完成 Workflow 动态更新无需重新创建实例

#### AgentRuntime - 核心分层调度
- 已完成 Agent 层 Master 实现
- 已完成 Agent 生命周期管理、状态监控、资源分配
- 已完成 故障转移基础
- 已完成 Workflow 层 Master 实现
- 已完成 Workflow 依赖关系计算
- 已完成 Workflow 调度决策和状态管理
- 已完成 Workflow 动态加载、卸载和版本更新
- 已完成 Node 层 Worker 池
- 已完成 Node 多线程并发执行
- 已完成 动态负载均衡
- 已完成 线程安全（std::atomic 替换 std::rand()）
- 已完成 优雅关闭（drain 模式 + 5s 超时）

#### 系统通信机制
- 已完成 Agent 间通信基础
- 已完成 微服务层接口封装
- 已完成 Request/Response 序列化
  - to_json/from_json 实现
  - to_binary/from_binary 实现
- 已完成 高速数据总线（InProcess）
- 已完成 ZeroCopyDataBus 订阅通知
- 待完成 零拷贝跨进程（POSIX/SystemV，P2 规划）
- 待完成 分布式 Agent 间通信

#### 监控和日志
- 已完成 基础监控接口
- 已完成 性能指标收集
  - 跨平台实现（Linux + macOS）
  - Mach API 实现（macOS）
- 已完成 日志系统
- 已完成 调试信息输出
- 待完成 分布式监控
- 待完成 第三方监控系统对接（Prometheus、Grafana）

#### 分布式执行支持
- 待完成 多 Agent 实例协同
- 待完成 负载均衡策略
- 待完成 容错和故障转移
- 待完成 分布式事务支持

---

### 第三阶段：基础设施（58% 完成）

#### Actor 机制
- 已完成 Actor 系统基础框架
- 已完成 Protoactor-cpp 适配实现
- 已完成 消息序列化和反序列化
- 已完成 Ask 模式实现
- 已完成 本地 Actor 通信
- 待完成 远程 Actor 通信
- 待完成 Actor 监督策略

#### 网络适配器
- 已完成 网络接口定义
- 已完成 私有协议实现
- 已完成 SSL 适配器接口
- 待完成 服务器/客户端完整实现
- 待完成 负载均衡支持
- 待完成 连接池管理

#### 事件循环适配器
- 已完成 EventLoop 接口定义
- 已完成 定时器支持
- 已完成 文件描述符事件
- 待完成 其他事件库适配（libuv、libev）

#### 序列化适配器
- 已完成 序列化接口定义
- 待完成 JSON 序列化优化
- 待完成 其他序列化格式支持

#### JSON 适配器
- 已完成 JSON 适配器接口定义
- 已完成 RapidJSON 适配器实现
- 已完成 Stub 实现（无依赖回退）
- 已完成 WorkflowParser JSON 支持

---

### 第四阶段：可选独立组件

#### DSL 工具项目
- 待完成 DSL 编辑器
- 待完成 DSL 可视化工具
- 待完成 DSL 语法高亮支持
- 待完成 DSL 实时验证

> 注意：DSL 解析和验证功能已在 AgentFlow 中实现（WorkflowParser）

#### 监控日志收集器
- 待完成 监控数据采集增强
- 待完成 日志数据汇聚
- 待完成 第三方运维系统对接
- 待完成 监控仪表板（Dashboard）
- 待完成 告警规则配置
- 待完成 告警通知机制

#### 后端服务
- 待完成 文件存储服务集成
- 待完成 大语言模型推理服务集成
- 待完成 其他第三方 Agent 或工具服务集成

---

## 待实现功能（按优先级）

### P0 - 核心功能（已全部完成）

所有 P0 缺陷已在 2026-02-18 修复：
- AgentRuntime 调度闭环
- 线程安全（atomic worker ID）
- C++ 函数执行
- FOR_EACH 循环
- 性能指标跨平台
- Request/Response 序列化

### P1 - 高优先级（已全部完成）

所有 P1 功能已在 2026-02-18 实现：
- ZeroCopyDataBus 订阅通知
- 条件表达式解析缓存
- 优雅关闭机制
- 本地数据通道

### P2 - 中优先级（下一步规划）

#### 性能优化
1. **零拷贝共享内存**
   - POSIX 共享内存真实实现
   - SystemV 共享内存支持
   - 跨进程数据总线

2. **调度器优化**
   - Worker 池按语言分组
   - 动态 Worker 池扩缩容
   - 智能负载均衡

3. **Workflow 优化**
   - Workflow 热替换
   - 动态更新无需重启
   - ZeroCopyDataBus 分段锁

#### 分布式支持
4. **跨进程通信**
   - 消息序列化完整实现
   - 远程 Actor 通信
   - 分布式调度

5. **高可用**
   - 故障检测和自动恢复
   - 状态复制和同步
   - 分布式协调（Raft/Paxos）

### P3 - 低优先级（长期规划）

#### 开发工具
1. **DSL 工具链**
   - 可视化编排工具
   - DSL 语法检查器
   - DSL 到 JSON 转换器

2. **调试工具**
   - Workflow 执行追踪
   - 可视化监控面板
   - 性能分析工具

#### 生态集成
3. **云原生支持**
   - Kubernetes 集成
   - Docker 容器化
   - Helm Chart

4. **第三方集成**
   - Prometheus 监控
   - Grafana 可视化
   - ELK 日志收集

### P4 - 外部依赖相关

1. **可选依赖完整实现**
   - Protoactor-cpp 远程通信
   - OpenSSL 完整 SSL/TLS

2. **新依赖引入评估**
   - gRPC 支持
   - HTTP/2 支持
   - MessagePack 序列化

---

## 近期开发重点

### 当前阶段（已完成）
- P0 缺陷修复（6/6）
- P1 功能补全（6/6）
- 跨平台构建支持
- 文档重组和刷新
- 代码结构重组

### 下一阶段（P2 性能优化）
1. **POSIX 共享内存实现**
   - 工作量：大
   - 优先级：高
   - 预计时间：2-3周

2. **Worker 池按语言分组**
   - 工作量：大
   - 优先级：高
   - 预计时间：1-2周

3. **Workflow 热替换**
   - 工作量：大
   - 优先级：中
   - 预计时间：2-3周

4. **修复预存在测试失败**
   - AgentFlowGoogleTests SEGFAULT
   - HotReloadTests 时序问题
   - EventLoopTests SEGFAULT
   - 工作量：中
   - 优先级：高

---

## 版本规划

### v0.1.0（当前版本）
- 核心框架实现
- 基础功能完成
- P0/P1 缺陷修复
- 跨平台支持

### v0.2.0（计划中）
- 目标：P2 性能优化
- 预计发布：2026 Q2
- 主要特性：
  - POSIX 共享内存
  - Worker 池优化
  - Workflow 热替换
  - 测试失败修复

### v0.3.0（规划中）
- 目标：分布式支持
- 预计发布：2026 Q3
- 主要特性：
  - 远程 Actor 通信
  - 分布式调度
  - 高可用支持

### v1.0.0（远期目标）
- 目标：生产就绪
- 预计发布：2026 Q4
- 主要特性：
  - 完整功能集
  - 生产级稳定性
  - 完整文档和工具链
  - 云原生支持

---

## 代码完备性评估

> 版本: 1.0
> 评估日期: 2026-02-17
> 评估方法: 对照功能清单，逐一审查源码实现

### 构建状态

| 项目 | 状态 | 说明 |
|------|------|------|
| 编译 | 通过 | 38个源文件全部编译成功 |
| 链接 | 通过 | agentflow 库和 8 个示例程序链接成功 |
| 测试运行 | 失败 | macOS dyld 找不到 libgtest_main.1.11.0.dylib（RPATH 问题） |
| 测试性能工具 | 已迁移 | `tests/unit/workflow/lib/performance_metrics.{h,cpp}` 已与测试代码同目录 |

### 文档与代码一致性审查

以下功能在文档中标记为已完成，但代码实际为 stub 或不完备：

#### 严重不一致（核心功能不可用）

**ZeroCopyDataBus POSIX/SystemV 共享内存**
- **文档声称**: "本地零拷贝共享具体实现 - 已实现 ZeroCopyDataBus，支持共享内存和读写锁"
- **代码现状**: `src/runtime/communication/zero_copy_databus.cpp` 中：
  - `SharedMemoryFactory::create_posix_shared()` -> 返回 nullptr
  - `SharedMemoryFactory::create_systemv_shared()` -> 返回 nullptr
  - 仅 `create_in_process()` (堆内存模拟) 可用，不是真正的共享内存
- **影响**: 跨进程零拷贝数据共享完全不可用
- **修复建议**: 使用 `shm_open()/mmap()` 实现 POSIX 共享内存块，或明确文档标注为 "仅进程内可用"

#### 中等不一致（功能部分缺失）

**Request/Response 反序列化**
- **文件**: `src/runtime/network/request.cpp`, `src/runtime/network/response.cpp`
- **问题**: `from_json()` 和 `from_binary()` 方法为空 stub
- **影响**: 无法从 JSON/Binary 格式恢复 Request/Response 对象
- **状态**: 已修复

**AgentRuntime 节点调度闭环断裂**
- **文件**: `src/runtime/scheduler/agent_runtime.cpp` 中 `on_node_complete()`
- **问题**: 方法获取了后续可执行节点列表，但没有将其入队到 worker 线程
- **影响**: 工作流图执行在第一批节点完成后停止
- **状态**: 已修复

**ZeroCopyDataBus 订阅通知未触发**
- **文件**: `src/runtime/communication/zero_copy_databus.cpp`
- **问题**: `subscribe()` 注册了回调，但 `put()` 方法中从未调用 `notify_subscribers()`
- **影响**: 数据变更订阅机制完全无效
- **状态**: 已修复

**ControlFlow FOR_EACH 循环**
- **文件**: `src/runtime/scheduler/control_flow.cpp`
- **问题**: `FOR_EACH_LOOP` 分支直接 `return false`
- **影响**: FOR_EACH 循环类型完全不可用
- **状态**: 已修复

**跨进程消息序列化**
- **文件**: `vendor/adapter/actor/message_serializer.cpp`
- **问题**: 仅 RAW 格式可用，跨进程序列化路径为 stub
- **影响**: Actor 消息无法跨进程传递
- **状态**: 待实现（P2）

**StorageManager 工厂函数未定义**
- **文件**: `src/runtime/storage/storage_manager.cpp`
- **问题**: `create_agent_storage()` 和 `create_workflow_storage()` 工厂函数体未实现
- **影响**: StorageManager 初始化时无法创建实际的存储实例
- **状态**: 待实现

### 代码 Bug 清单

#### BUG-001: std::rand() 线程安全问题
- **文件**: `src/runtime/scheduler/agent_runtime.cpp`
- **严重度**: 中
- **描述**: `get_least_loaded_worker()` 中使用 `std::rand()`，该函数非线程安全
- **状态**: 已修复 - 使用 std::atomic<int> 替换

#### BUG-002: execute_with_timeout lambda 引用悬空
- **文件**: `src/workflow/node/node.cpp` 中 `execute_with_timeout()`
- **严重度**: 中
- **描述**: lambda 捕获 `executor` 引用，如果超时后线程仍在运行，executor 生命周期结束后引用悬空
- **状态**: 已修复

#### BUG-003: on_node_complete 调度闭环断裂
- **文件**: `src/runtime/scheduler/agent_runtime.cpp` 中 `on_node_complete()`
- **严重度**: 中（功能性 Bug）
- **描述**: 获取后续可执行节点后未入队调度
- **状态**: 已修复

### 测试覆盖度评估

#### 当前测试统计

| 测试文件 | 用例数 | 覆盖组件 | 质量 |
|---------|--------|---------|------|
| test_local_channel_gtest.cpp | 43 | LocalChannel 完整功能 | 完善 |
| test_hot_reload.cpp | 44 | 多语言热重载、版本管理 | 完善 |
| test_workflow_json_gtest.cpp | 29 | Workflow JSON 解析 | 完善 |
| test_agent_json_gtest.cpp | 27 | Agent JSON 解析 | 完善 |
| test_adapter_isolation.cpp | 6 | Actor 适配器隔离 | 基本 |
| test_node_gtest.cpp | 5 | Node 基本创建和状态 | 基础 |
| test_agent_gtest.cpp | 3 | Agent 创建 | stub |
| test_workflow_gtest.cpp | 3 | Workflow 创建 | stub |
| **合计** | **258** | | |

#### 缺失测试（按优先级排序）

**必须补充（P0）**

| 组件 | 需要测试的内容 |
|------|--------------|
| **AgentRuntime** | 三层调度、workflow 执行、node 并发调度、负载均衡 |
| **Workflow 执行** | 工作流图拓扑排序、get_executable_nodes、mark_node_executed、循环/条件 |
| **Agent 生命周期** | 状态机转换（CREATED->INITIALIZED->RUNNING->PAUSED->STOPPED） |
| **ControlFlowManager** | 条件评估、循环控制、错误处理策略 |
| **DataBus** | put/get/remove、订阅回调、线程安全、容量限制 |

### 代码优化建议

#### 架构层面优化

**OPT-A1: Worker 池按语言分组**
- **当前问题**: 所有 Node（C++/Python/Shell）共享同一个 worker 线程池
- **建议**: 按 ExecutionMode 分组，C++ 节点使用高优先级池，脚本节点使用隔离池
- **优先级**: P2

**OPT-A2: 优雅关闭机制**
- **当前问题**: `AgentRuntime::stop()` 直接标记 `running_ = false`，不等待进行中的 workflow 完成
- **建议**: 实现 drain 模式：停止接受新任务 -> 等待进行中任务完成 -> 关闭 worker 线程
- **优先级**: P1
- **状态**: 已完成

**OPT-A3: Workflow 热替换**
- **当前问题**: `Workflow::update_dynamic()` 仅调用 `load_from_dsl()`，需要重建实例
- **建议**: 支持原地更新工作流图定义，保持执行状态
- **优先级**: P2

#### 性能层面优化

**OPT-P1: 线程安全随机数**
- **当前问题**: `std::rand()` 非线程安全
- **建议**: 使用 `thread_local std::mt19937`
- **优先级**: P0
- **状态**: 已完成

**OPT-P2: 条件表达式解析缓存**
- **当前问题**: `Workflow::parse_condition_expression()` 每次评估都重新解析条件字符串
- **建议**: 首次解析后缓存 `ConditionExpression` 对象
- **优先级**: P1
- **状态**: 已完成

**OPT-P3: ZeroCopyDataBus 分段锁**
- **当前问题**: InProcessSharedMemoryBlock 使用单一 `shared_mutex`，高并发下写操作会阻塞所有读操作
- **建议**: 按 key 的 hash 分段，每段独立锁，降低锁竞争
- **优先级**: P2

**OPT-P4: DataBus 容量预分配**
- **当前问题**: `HighSpeedDataBus` 的 `std::unordered_map` 在数据量增长时频繁 rehash
- **建议**: 构造时根据 `max_capacity` 预留 bucket 数量 (`reserve()`)
- **优先级**: P3

### 优化实施路线

#### 第一阶段：P0 缺陷修复（已全部完成）

| 编号 | 任务 | 涉及文件 | 状态 |
|------|------|---------|------|
| OPT-B1 | performance_metrics.cpp 加入编译 | CMakeLists.txt | 已完成 |
| OPT-B2 | 修复 gtest RPATH | CMakeLists.txt | 已完成 |
| OPT-P1 | std::rand() -> std::atomic<int> | agent_runtime.cpp | 已完成 |
| OPT-R1 | on_node_complete 调度闭环 | agent_runtime.cpp | 已完成 |
| BUG-002 | lambda 引用悬空修复 | node.cpp | 已完成 |

#### 第二阶段：P1 功能补全（已全部完成）

| 编号 | 任务 | 涉及文件 | 状态 |
|------|------|---------|------|
| OPT-R5 | execute_cpp_function 真实实现 | node.cpp, node.h | 已完成 (cpp_function 回调 + CodeLoader 回退) |
| OPT-R3 | ZeroCopyDataBus 订阅通知 | zero_copy_databus.cpp | 已完成 (put()后调用notify_subscribers) |
| OPT-R4 | Request/Response 反序列化 | request.cpp, response.cpp | 已完成 (from_json + from_binary) |
| OPT-R2 | FOR_EACH 循环实现 | control_flow.cpp | 已完成 (逗号分隔集合迭代) |
| OPT-P2 | 条件表达式解析缓存 | workflow.cpp, workflow.h | 已完成 (unordered_map缓存) |
| OPT-A2 | 优雅关闭机制 | agent_runtime.cpp, agent_runtime.h | 已完成 (drain模式+超时) |
| 测试 | 修复 NodeLifecycle 测试 | test_node_gtest.cpp | 已完成 |

#### 第三阶段：P2 性能与架构优化（建议中期规划）

| 编号 | 任务 | 涉及文件 | 工作量 |
|------|------|---------|--------|
| 2.1.1 | POSIX 共享内存真实实现 | zero_copy_databus.cpp | 大 |
| OPT-A1 | Worker 池按语言分组 | agent_runtime.cpp | 大 |
| OPT-A3 | Workflow 热替换 | workflow.cpp | 大 |
| OPT-P3 | ZeroCopyDataBus 分段锁 | zero_copy_databus.cpp | 中 |
| 2.2.5 | 跨进程消息序列化 | message_serializer.cpp | 大 |
| 2.2.6 | StorageManager 工厂函数 | storage_manager.cpp | 中 |

---

## 代码质量目标

### 测试覆盖
- 已完成 单元测试框架（GTest）
- 已完成 集成测试（6/10 通过）
- 已完成 性能测试框架
- 待完成 代码覆盖率 > 80%
- 待完成 所有核心模块单元测试
- 待完成 端到端测试套件

### 代码规范
- 已完成 C++17 标准
- 已完成 Google C++ 风格指南
- 已完成 clang-format 配置
- 已完成 clang-tidy 静态分析
- 已完成 Cppcheck 配置
- 待完成 代码审查流程
- 待完成 贡献者指南完善

### CI/CD
- 已完成 GitHub Actions 工作流
- 已完成 多平台构建（Linux/macOS AMD64/ARM64）
- 已完成 自动化测试
- 已完成 代码质量检查
- 已完成 自动化发布
- 待完成 Docker 镜像构建
- 待完成 性能基准测试自动化

---

## 文档目标

### 用户文档
- 已完成 README.md（项目概览）
- 已完成 快速入门指南
- 已完成 API 参考手册
- 已完成 架构设计文档
- 待完成 用户手册（完整版）
- 待完成 最佳实践指南
- 待完成 故障排查手册

### 开发者文档
- 已完成 开发指南
- 已完成 贡献指南
- 已完成 测试指南
- 已完成 CI/CD 指南
- 待完成 设计模式文档
- 待完成 性能优化指南
- 待完成 插件开发指南

### 示例和教程
- 已完成 基础示例（hello_world 等）
- 待完成 进阶示例
- 待完成 分布式部署示例
- 待完成 视频教程
- 待完成 在线演示环境

---

## 构建和部署目标

### 构建系统
- 已完成 CMake 构建系统
- 已完成 跨平台支持（Linux/macOS）
- 已完成 依赖管理
- 已完成 构建脚本（build.sh）
- 待完成 Windows 支持
- 待完成 交叉编译支持
- 待完成 构建缓存优化

### 部署方式
- 已完成 本地部署
- 待完成 Docker 容器化
- 待完成 Kubernetes 部署
- 待完成 云平台支持（AWS、Azure、GCP）
- 待完成 一键部署脚本

### 发布管理
- 已完成 版本号管理
- 已完成 GitHub Release
- 已完成 变更日志
- 待完成 软件包管理（apt、yum、brew）
- 待完成 二进制分发优化

---

## 变更记录：移除网络模块

### 移除内容

**2026-02-21**: 移除了 `src/runtime/network` 目录，包括：
- `src/runtime/network/private_protocol.cpp` - 私有协议编码解码器
- `src/runtime/network/request.cpp` - Request 类实现
- `src/runtime/network/response.cpp` - Response 类实现
- `tests/unit/workflow/cpp/test_request_response_gtest.cpp` - Request/Response 测试用例（56个测试）

### 移除原因

网络模块（Request/Response/PrivateProtocol）在当前阶段不是核心功能，优先级较低。移除该模块以：
1. 简化核心功能结构
2. 减少代码维护负担
3. 专注于 Agent、Workflow、Runtime 等核心模块的完善
4. 为将来的网络功能设计留出空间

### 后续计划

- 网络通信功能可在未来版本中重新设计和实现
- 将根据实际需求确定通信协议和 API 设计

---

## 最新变更总结 (2026-02-21)

### 事件适配器 (Event Adapter)

**新增内容：**
- `include/internal/vendor/event_adapter.h` - 事件适配器接口定义
- 支持将 Agent 运行时事件转换为可观察的事件流
- 提供统一的事件发送和订阅机制

**功能特性：**
- 事件类型：Agent 生命周期事件（启动、停止、错误等）
- 事件过滤和优先级支持
- 异步事件处理机制

### Agent 组件完善

**更新内容：**
- Agent 组件加强了生命周期管理
- 改进了 Workflow 任务调度和执行
- 完善了错误处理和日志记录机制

### 示例代码重组

**调整内容：**
- 将 `examples/dsl` 目录中的所有 DSL 示例文件移动到 `examples/workflow/dsl/`
- 包含文件：
  - `conditional_workflow.json` - 条件流程示例
  - `loop_workflow.json` - 循环流程示例
  - `sample_workflow.json` - 基础流程示例
  - `visualizer.html` - 流程可视化工具
- 目的：统一整理示例代码结构，使 DSL 示例与 Workflow 示例位置更加紧密

**新的目录结构：**
```
examples/
├── workflow/
│   ├── dsl/                      # DSL 相关示例（新位置）
│   │   ├── conditional_workflow.json
│   │   ├── loop_workflow.json
│   │   ├── sample_workflow.json
│   │   └── visualizer.html
│   ├── dsl_example.cpp
│   └── node_execution_example.cpp
├── agent/
├── adapter_example.cpp
├── basic_example.cpp
├── metrics_example.cpp
├── p1_features_example.cpp
├── storage_example.cpp
└── README.md
```

---

## 更新日志

| 日期 | 变更 |
|------|------|
| 2026-02-21 | 刷新 Agent/Runtime/Workflow 当日优化快照：补充消息链路、master/worker 全局调度、内建 workflow 资产化与全量测试结果（ctest 10/10）并更新下一步优化方向 |
| 2026-02-21 | 新增事件适配器 (Event Adapter) 和示例代码重组：DSL 示例从 examples/dsl 移动到 examples/workflow/dsl |
| 2026-02-21 | 移除网络模块 (Request/Response/PrivateProtocol) 以简化核心功能结构，优先专注于 Agent、Workflow、Runtime 等核心模块 |
| 2026-02-21 | 完成网络协议和Request/Response实现：Protocol Encoder/Decoder、Request/Response类、JSON序列化支持 |
| 2026-02-18 | 合并 roadmap.md 和 code-review.md 到 planning.md，整合项目规划文档 |
| 2026-02-17 | 代码完备性评估：修正功能状态标注，标记 stub/不完备功能 |
| 2026-02-17 | 整理功能规划文档，更新至 docs 目录 |
| 2026-02-16 | 重新梳理功能实现状态，更新优先级排序 |

---

## Refactoring Iteration 3 - Agent Framework Enhancement & Build System

### Overview
This iteration focuses on enhancing the agent framework with practical examples, improving the publish/build system, and documenting core enhancements made to the node adapter and session management components.

### Changes Implemented

#### 1. Core Enhancements in `src/agent`

**Node Adapter (`node_adapter.cc`, `node_adapter.so`)**
- Implemented complete node execution environment for agents
- Added support for multiple node types: start, end, task, condition, loop
- Implemented data flow between connected nodes
- Added error handling and node state management
- Compiled as shared library (`node_adapter.so`) for runtime loading

**Session Management (`session.h`, `session.cc`)**
- Implemented `AgentSession` class for managing agent lifecycle
- Added session state tracking (IDLE, RUNNING, PAUSED, COMPLETED, FAILED)
- Implemented context preservation across multiple agent invocations
- Added session storage and retrieval mechanisms

**Task Management (`task.h`, `task.cc`)**
- Implemented `Task` class for individual workflow tasks
- Added task scheduling and execution tracking
- Implemented task dependency resolution
- Added input/output parameter handling

#### 2. Enhanced `publish` Directory Structure

**`publish/agent_build.sh`**
- Enhanced script with support for parameterized agent building
- Added source directory and output zip file specification
- Supports building any agent from `examples/agent/*` directories
- Creates distributable zip packages with all necessary files
- New usage: `./publish/agent_build.sh build <source> <output>`

**Benefits:**
- Decoupled from hardcoded publish/agent directory
- Flexible build targeting for multiple agent examples
- Easy packaging for agent distribution

#### 3. New `examples/agent` Examples

**Directory Structure:**
```
examples/agent/
├── basic_agent/
│   ├── package.json          # Agent metadata
│   └── workflow.json         # Basic workflow DSL example
├── calculator_agent/
│   ├── package.json          # Agent metadata
│   ├── workflow.json         # Calculator workflow DSL
│   └── calc.py              # Python implementation
└── sample_package/
    └── package.json         # Reference sample
```

**`basic_agent` Example**
- Simple entry-level agent example
- Demonstrates basic workflow structure with 3 nodes (start, process, end)
- Package metadata in JSON format
- Supports easy extension with new nodes

**`calculator_agent` Example**
- Advanced example with data flow and validation
- Demonstrates multi-step workflow: validate input → add numbers → end
- Includes Python implementation (`calc.py`) for actual computation
- Shows parameter passing between workflow nodes
- Example usage: `./examples/agent/calculator_agent/calc.py 5 3` → Output: `5 + 3 = 8`

#### 4. Build System Integration

**Build Verification:**
```bash
# Build examples
./build.sh build

# Run tests
./build.sh test

# Package agents
./publish/agent_build.sh build examples/agent/basic_agent build/basic_agent.zip
./publish/agent_build.sh build examples/agent/calculator_agent build/calculator_agent.zip
```

**Generated Artifacts:**
- `build/basic_agent.zip` - Packaged basic agent (785 bytes)
- `build/calculator_agent.zip` - Packaged calculator agent (1.3 KB)

### Documentation Updates

- Added comprehensive agent example documentation
- Updated build system documentation
- Documented node adapter architecture
- Created workflow DSL examples for reference

### Related Components

**Files Modified/Created:**
- `src/agent/node_adapter.cc` - Node execution engine
- `src/agent/session.h/cc` - Session management
- `src/agent/task.h/cc` - Task execution
- `publish/agent_build.sh` - Enhanced build script
- `examples/agent/basic_agent/*` - Basic agent example
- `examples/agent/calculator_agent/*` - Calculator agent example
- `docs/REFACTORING.md` - This documentation

### Testing

All examples have been validated:
- ✅ Directory structure creation
- ✅ JSON files validation (package.json, workflow.json)
- ✅ Python script execution (calc.py adds two numbers correctly)
- ✅ agent_build.sh script execution
- ✅ Zip package creation and contents verification
- ✅ Build system integration (./build.sh build, ./build.sh test)

### Future Work

- Implement C++ versions of agent adapters
- Add more complex workflow examples (loops, conditions)
- Implement workflow visualization tools
- Add agent testing framework
- Create agent deployment documentation

---

## 相关文档

- [架构设计](ARCHITECTURE.md) - Framework/Runtime/DataBus 架构
- [开发指南](../CONTRIBUTING.md) - 开发和构建说明
- [测试指南](README.md) - 测试方法
- [贡献指南](../CONTRIBUTING.md) - 贡献流程

---

**项目维护者**: @DanielKernel

## Agent 包管理与加载重构 (2026-02-21)

### 功能概述
为了支持 Agent 的包管理与加载功能，进行了以下重构工作：
1. **支持目录方式加载 Agent**：
   - 在 `src/agent` 下新增 `package.h` 和 `package.cpp`，支持解析 `package.json` 和加载 Agent 包目录。
   - `package.json` 定义了 Agent 的名称、版本、入口点和工作流文件列表。
2. **简化 Agent 实例化**：
   - 在 `Agent` 类中新增 `create_from_package` 静态工厂方法，支持直接从目录创建 Agent 实例。
   - 更新了 `Agent` 类的成员函数，支持加载多个工作流。
3. **构建系统更新**：
   - 更新 `CMakeLists.txt` 链接 `dl` 库，支持动态加载。
   - 新增 `test_agent_loading` 测试目标。
4. **功能验证**：
   - 新增 `tests/functional/src/test_agent_loading.cpp` 功能测试，验证 Agent 包加载、生命周期管理和工作流执行。
   - 修复了 `AgentState` 命名空间引用的编译错误。
5. **示例增强**：
   - 在 `examples/agent` 目录下新增 `basic_agent` 和 `calculator_agent` 示例包，包含完整的 `package.json` 和 `workflow.json`。

### 文件变更
- `include/internal/agent/package.h` (新增)
- `src/agent/package.cpp` (新增)
- `include/external/agent/agent.h` (修改)
- `src/agent/agent.cpp` (修改)
- `CMakeLists.txt` (修改)
- `tests/functional/src/test_agent_loading.cpp` (新增)
- `examples/agent/basic_agent/workflow.json` (新增)
- `examples/agent/calculator_agent/workflow.json` (新增)

### 2026-06-25 Agent核心重构与事件系统合并

1. **运行时清理**：
   - 移除了未使用的 `src/runtime/network` 目录。
   - 删除了旧的 `src/runtime/event.cpp` 和 `include/internal/vendor/event/event.h`，将事件管理逻辑合并至 `AgentEvent`。
   - 删除了 `Testing` 目录及冗余的测试目录 (`tests/unit/workflow`, `tests/unit/runtime`, `tests/unit/agent`)。

2. **事件系统增强**：
   - 基于 `libevent` 实现了 `EventAdapter` (`vendor/adapter/event/libevent_adapter.cpp`)。
   - `AgentEvent` 集成了适配器模式，支持同步和基于 `libevent` 的异步事件分发。
   - 更新了 `tests/unit/src/test_event_gtest.cpp` 以测试新的 `AgentEvent` 类。

3. **Agent 核心组件增强**：
   - 实现了 `AgentSession` (会话管理), `AgentTask` (任务管理), `AgentMessages` (消息队列), `AgentContext` (上下文管理)。
   - `Agent` 类集成所有新组件，并在 `initialize_components` 中统一初始化。
   - 更新了 `src/agent/agent.cpp` 和 `include/external/agent/agent.h`。

4. **示例与目录重构**：
   - 将 `examples/dsl` 目录重构为 `examples/workflow`，包含原来的 DSL 示例和 JSON 工作流文件。
   - 将 `visualizer.html` 移至 `publish/agent` 目录。
   - 确保 `examples/agent` 包含 Agent 包示例 (`basic_agent`, `calculator_agent`)。

### 文件变更
- `src/runtime/event.cpp` (删除)
- `include/internal/vendor/event/event.h` (删除)
- `src/agent/session.cpp` / `include/internal/agent/session.h` (新增/更新)
- `src/agent/task.cpp` / `include/internal/agent/task.h` (新增/更新)
- `src/agent/messages.cpp` / `include/internal/agent/messages.h` (新增/更新)
- `src/agent/context.cpp` / `include/internal/agent/context.h` (新增/更新)
- `src/agent/event.cpp` / `include/internal/agent/event.h` (更新)
- `src/agent/agent.cpp` (更新)
- `CMakeLists.txt` (更新)

### 2026-02-22 Agent 发布规范重构

1. **Agent 包规范落地**
   - 在 `publish/agent/AGENT_PACKAGE_SPEC.md` 新增 Agent 双层打包规范说明。
   - 在 `publish/agent` 补充 `agent.json`、`workflow_01.json`、`node_demo.sh` 模板。

2. **打包脚本升级**
   - `publish/agent_build.sh` 从 zip 产物调整为双层 `tar.gz` 产物：
     - `$xxxAgent_$PKGTYPE_$PKGVER.tar.gz`
     - `$xxxAgent_$AGTVER.tar.gz`

3. **开发文档重写**
   - `publish/README.md` 改为业务 Agent 包开发指南，覆盖 Agent/Workflow/Node 开发流程。
   - 在 `publish/workflow` 新增 Node 规范与 API 手册：
     - `NODE_DEVELOPMENT_SPEC.md`
     - `NODE_API_REFERENCE.md`

4. **Workflow 样例迁移**
   - `workflow_sample.jsonc` 迁移到 `publish/workflow/workflow_sample.jsonc`。
   - `src/workflow/dsl/README.md` 已更新引用路径。

5. **示例刷新**
   - 更新 `examples/agent/basic_agent` 与 `examples/agent/calculator_agent`，补充规范化示例文件并保留现有加载兼容字段。

### 2026-02-22 Workflow Edge 控制与模式化重构

1. **Edge 控制统一化**
   - 扩展 Edge 配置，新增 `control.type + control.*` 机制，支持 `if/loop/fork/join/async/goto`。
   - DSL 解析器支持新控制字段，并保留 `condition_expression` 向 `if` 控制映射的兼容行为。

2. **Workflow 流程能力增强**
   - 执行路径增加 edge 控制判断（含 async 软依赖、loop/goto 控制判定）。
   - 工作流图校验与拓扑排序对 `loop/goto` 控制边放宽，支持显式环回流程。

3. **模式资产新增**
   - 新增 `publish/workflow/pattern/serial_workflow.json`。
   - 新增 `publish/workflow/pattern/parallel_workflow.json`。
   - 新增 `publish/workflow/pattern/cycled_workflow.json`。

4. **Node 模式模板新增**
   - `publish/workflow/node_patterns/` 新增问答/API工具/MCP/skills 四类模板文档。

5. **文档同步**
   - 更新 `publish/README.md`、`publish/workflow/README.md`、`publish/workflow/workflow_sample.jsonc`、`src/workflow/dsl/README.md`。

6. **测试增强**
   - `tests/unit/src/test_workflow_json_gtest.cpp` 增加 edge control 解析与 goto 环回流程解析测试。

### 2026-02-22 Workflow 动态生成与嵌套重构

1. **Node/Workflow Skill 化**
   - 新增 `src/workflow/node/skill.md`，定义 node 能力说明、调用方法、输入输出契约。
   - 新增 `src/workflow/dsl/skill.md`，定义 workflow 编排技能、include 调用方式与控制约定。

2. **嵌套 Workflow 支持**
   - 扩展 `Node::Config`，新增 `workflow_ref` 字段，支持 `type: "workflow"` 节点引用子工作流。
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

### 2026-02-22 Workflow 重构后兼容性巡检修复

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
  - `include/internal/agent/agent.h` 承接 Agent 声明能力，保证内部编译路径稳定。
- 文档与测试：
  - `examples/README.md` 改为 agent 包视角说明，补充 `AgentService.config`、自动加载与请求处理链路说明；
  - `tests/functional/src/test_agent_loading.cpp` 新增 AgentActor + RuntimeActor 自动加载/注册链路测试（RapidJSON 开启时执行）。
