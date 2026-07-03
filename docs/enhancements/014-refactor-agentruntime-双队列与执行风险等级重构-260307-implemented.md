## 2026-03-07 agent/runtime 双队列与执行风险等级重构

### 本次重构

- agent 侧：
  - session 删除自建收发队列，消息统一下沉到 dispatcher/coordinator task；
  - 每个 task 拥有 `intent_request` / `data_exchange` 双队列，以及超时监控/重发线程；
  - `SessionRouter` 负责默认请求接收、响应发送、路由线程和 session 老化线程。
- runtime 侧：
  - worker 改为 `control` / `business` 双队列模型；
  - workflow / node / edge 不再保留独立事件线程，执行控制统一收敛到 worker；
  - runtime 维护 `task -> workflow instance -> worker` 映射，并将 worker 回包回流到 task 数据队列。
- workflow 侧：
  - node 新增 `execution_risk_level`（`green` / `yellow` / `red`）；
  - workflow 在解析时聚合红灯节点，并暴露 `is_red_light()` 供 coordinator 判别 skill 是否可直接执行。
- tests/docs/examples：
  - 新增 workflow risk-level 解析测试；
  - 更新 multi-session-chat 文档与 package 说明，明确 session 池、skill 实例池和 task 双队列模型；
  - 清理示例目录遗留的 `.bak` 脚本文件。

### 验证结果

- `./build.sh build`：通过
- `ctest --test-dir build -R 'AgentTests|E2EMessageRouting' --output-on-failure`：通过
- `./build.sh test`：通过
