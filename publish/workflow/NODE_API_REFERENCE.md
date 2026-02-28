# Node API 参考

## 运行时 API 约定

Node 实现应遵循以下运行时约定：

1. 调用输入包含基础元信息（如 `trace_id/workflow_id/node_id`）。
2. 输入输出数据通过 JSON `payload` 交互，并与 workflow 的 `inputs/outputs` 对齐。
3. 重试与超时由 workflow node 配置控制（`max_retries`、`retry_delay_ms`、`timeout_ms`）。

## 执行模式映射

- `shell_script`：通过 shell 执行命令
- `executable`：直接执行二进制
- `go_script`：执行 Go 脚本
- `python_script`：执行 Python 脚本
- `java_jar`：执行 Java JAR
- `so_library`：加载动态库并调用指定符号
- `cpp_function`：调用框架注册的 C++ 函数

以上执行模式统一由 Worker 执行引擎分发，Node 负责声明执行模式、命令、参数与 I/O 约束。

## 输出示例

```json
{
  "status": "SUCCESS",
  "payload": {
    "result": "ok"
  }
}
```
