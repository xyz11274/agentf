## MMNode 热更新能力下沉到 Worker（2026-02-28）

- 热更新控制面迁移到 Worker：
  - `include/internal/runtime/worker.h` 新增 `check_and_reload_code/get_code_version/rollback_code_version/stop_hot_reload`；
  - `src/runtime/worker/worker.cpp` 统一执行与热更新能力，支持运行中版本更新与回退。
- MMNode 热更新 API 保持兼容但改为 Worker 委托：
  - `src/workflow/node/node.cpp` 中 `reload/check/version/rollback` 统一转发到 Worker；
  - 外部执行请求携带 `enable_hot_reload/hot_reload_config`，默认配置无需额外 `enable_code_reload` 调用。
- 代码归属调整：
  - `src/workflow/node/exe_loader.cpp` 迁移到 `src/runtime/worker/exe_loader.cpp`，热更新实现与 Worker 执行引擎同域管理。
- 测试补充：
  - `tests/unit/src/test_runtime_master_worker_gtest.cpp` 新增 Worker 热更新+回退用例；
  - `tests/unit/src/test_hot_reload.cpp` 刷新 MMNode 场景为默认热更新语义。
