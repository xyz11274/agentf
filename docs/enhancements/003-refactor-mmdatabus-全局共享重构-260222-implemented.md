## MMDataBus 全局共享重构（2026-02-22）

- MMDataBus 增强为“本地缓存优先 + 远端服务同步”模型：支持进程内、跨进程、跨节点共享链路。
- 新增 DataRef 流程：`store_with_ref / resolve_ref / find_refs_by_owner`。
- owner 维度索引支持：`request_id/user_id/client_id/agent_id/session_id/task_id/workflow_id/node_id/master_id/worker_id`。
- `msgque` 控制面仅携带 `data_ref`，payload 由 MMDataBus 本地缓存读取，本地 miss 自动回源并回填。
- Redis 依赖接入：`vendor/vendor.conf` 增加 redis 仓库条目，新增 `vendor/adapter/cache/redis_adapter.cpp` 适配实现。
- 回归结果：
  - `./build.sh build` 通过；
  - `./build.sh test` 存在既有 HotReload 并行抖动；
  - `ctest --test-dir build --output-on-failure -j 1` 串行 10/10 通过。
