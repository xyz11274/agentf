# Hot Reload 与 Agent Package 更新设计

> **层级**：L4 · `architecture/modules/hot-reload-and-package-update.md`  
> **状态**：当前策略  
> **更新日期**：2026-07-03

## 1. 三种更新机制

| 机制 | 监听/更新对象 | 当前策略 | 是否影响 WS |
|------|---------------|----------|-------------|
| Bridge 开发 reload | `frontend/bridge/` Python 源码 | 开发启用，目录受限 | reload 时会断开 WS |
| Worker executable hot-reload | 节点 `.py/.sh/.so` command | 默认关闭，显式 opt-in | 不直接管理 WS |
| Agent package 更新 | 上传后 daemon `agent.unload → agent.load` | 保留 | Bridge 进程不重启，WS 不应断开 |

三者必须独立，不得用 Worker executable hot-reload 代替 Agent package 更新。

## 2. Bridge reload 边界

开发启动固定使用：

```text
--reload --reload-dir <repo>/frontend/bridge
```

skill 工作空间和 Agent package 位于 `examples/agent/`。任务在其中产生源码、图片、HTML、
临时文件或其他中间产物时，不属于 Bridge reload 范围，不会触发 Bridge 进程重启。

生产启动不传递 `--reload`，不存在文件监听子进程。

## 3. Worker executable hot-reload

当前 loader 在节点初始化/执行路径存在无条件 `reload_code()`、相对 command key 与共享全局
loader 等问题。在路径规范化、变更检测及多 package 隔离完成前，默认值设为 `false`。

需要实验该能力的节点必须显式配置：

```json
{"enable_hot_reload": true}
```

即使显式启用，Watcher 也只应关注配置的 executable 文件。skill 生成的普通中间文件不应触发
可执行代码 reload；但当前实现仍不建议在生产启用。

## 4. Package 上传更新不变量

`PUT /api/packages/{name}/update` 完成文件替换后：

1. 若 Agent 未加载，只更新磁盘内容；
2. 若 Agent 已加载，Bridge 调用 daemon `agent.unload` 后再调用 `agent.load(package_path)`；
3. 该路径不读取节点的 `enable_hot_reload`，因此关闭 Worker hot-reload 不影响 package 更新；
4. 更新失败必须保留 daemon 错误日志，不得将“磁盘写入成功”误报成“运行实例更新成功”。

## 5. Artifact 与任务成功语义

Artifact 是可选输出，不是所有用户请求或 workflow 的成功前提。统一规则如下：

- 请求/计划不要求 Artifact：没有 Artifact 可以正常成功；
- 请求明确要求 Artifact，且执行路径接受了该要求：最终必须生成 Artifact，或明确报告未生成原因；
- Artifact 已生成但后续文本整理失败：保留并返回已有 Artifact，同时报告降级；
- 禁止在没有 Artifact 证据时声称“图片仍可查看”。

因此判断依据是“请求与执行计划承诺”，不是简单检查 `skill_artifacts` 是否为空。

