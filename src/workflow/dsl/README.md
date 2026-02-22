# AgentFlow 工作流 DSL 参考手册

## 概览

AgentFlow 工作流 DSL 是一个 JSON（或 JSONC —— 带注释的 JSON）文档，完整描述了由类型化边连接的有向无环图 (DAG) 的计算节点。

规范参考示例是 [`publish/workflow/workflow_sample.jsonc`](../../../publish/workflow/workflow_sample.jsonc)。该文件中的每个字段都与 `src/workflow/dsl/workflow_parser.cpp` 中的 `JsonHelper` 调用一一对应。

---

## 顶层结构

```jsonc
{
  "version": "1.0",            // 必需 – DSL 模式版本
  "id":      "my_workflow",    // 必需 – 唯一的工作流标识符
  "name":    "My Workflow",    // 可选 – 显示名称
  "description": "…",         // 可选
  "dsl_file":    "path/to.jsonc",  // 可选 – 存储以便追踪
  "metadata": { … },          // 可选 – 自由格式，不被解析
  "nodes": [ … ],             // 必需 – 节点对象数组
  "edges": [ … ]              // 必需 – 边对象数组
}
```

---

## 节点字段

| 字段 | 类型 | 默认值 | 描述 |
|-------|------|---------|-------------|
| `id` | 字符串 | **必需** | 工作流中的唯一节点标识符 |
| `name` | 字符串 | `id` | 人类可读的显示名称 |
| `type` | 字符串 | `"default"` | 节点类别（例如 `"source"`、`"transform"`、`"sink"`） |
| `priority` | 整数 | `0` | 调度优先级；较高的值 = 更早执行 |
| `execution_mode` | 枚举字符串 | `"cpp_function"` | 节点如何执行（见下文） |
| `command` | 字符串 | `""` | 外部执行模式的脚本/二进制文件路径 |
| `working_dir` | 字符串 | `""` | 外部执行的工作目录 |
| `args` | 字符串数组 | `[]` | 附加到 `command` 的额外参数 |
| `function_name` | 字符串 | `""` | `so_library` 模式下导出的符号名称 |
| `main_class` | 字符串 | `""` | 用于将来 JAR 模式的 Java 主类 |
| `inputs` | 字符串数组 | `[]` | 命名的输入数据端口 |
| `outputs` | 字符串数组 | `[]` | 命名的输出数据端口 |
| `max_retries` | 整数 | `3` | 失败时的重试次数 |
| `retry_delay_ms` | 整数 | `1000` | 重试之间的延迟（毫秒） |
| `timeout_ms` | 整数 | `30000` | 单次尝试的实时超时（`0` = 无限制） |
| `memory_limit_mb` | 整数 | `0` | 内存上限（MiB）（`0` = 无限制） |
| `cpu_limit` | 整数 | `0` | CPU 上限（单核百分比）（`0` = 无限制） |
| `enable_hot_reload` | 布尔值 | `true` | 参与实时 SO 库热重新加载 |

### `execution_mode` 值

| 值 | 行为 |
|-------|-----------|
| `"cpp_function"` | 调用已注册的 C++ lambda（`Config::cpp_function`） |
| `"shell_script"` | 通过 `/bin/sh` 在 `working_dir` 中运行 `command` |
| `"executable"` | 作为直接子进程运行 `command` |
| `"python_script"` | 使用系统 Python 解释器运行 `command` |
| `"so_library"` | 使用 `dlopen(command)` 并调用符号 `function_name` |

---

## 边字段

| 字段 | 类型 | 默认值 | 描述 |
|-------|------|---------|-------------|
| `id` | 字符串 | **必需** | 唯一的边标识符 |
| `source` | 字符串 | **必需** | 上游节点的 `id` |
| `target` | 字符串 | **必需** | 下游节点的 `id` |
| `source_outputs` | 字符串数组 | `[]` | 要转发的 `source` 的输出端口 |
| `target_inputs` | 字符串数组 | `[]` | 要接收数据的 `target` 的输入端口 |
| `transfer_mode` | 枚举字符串 | `"shared_memory"` | 数据传输机制（见下文） |
| `data_formats` | 字符串数组 | `[]` | MIME 类型或自定义格式标签 |
| `supported_events` | 字符串数组 | `[]` | 此边反应的生命周期事件 |
| `condition_expression` | 字符串 | `""` | CEL 表达式；空 = 始终活跃 |
| `control` | 对象 | `{}` | 统一流程控制对象，含 `type` 和控制参数 |
| `priority` | 整数 | `0` | 在出边中的平手优先级 |

### `control` 对象

- `type`: `if` / `loop` / `fork` / `join` / `async` / `goto`
- 典型参数：
  - `expression`（if）
  - `loop_id` / `max_iterations`（loop）
  - `fork_group`（fork）
  - `join_group`（join）
  - `callback_node`（async）
  - `goto_target`（goto）

### `transfer_mode` 值

| 值 | 行为 |
|-------|-----------|
| `"shared_memory"` | 零复制的进程内指针传递（默认） |
| `"message_queue"` | 通过异步有界队列进行序列化 |
| `"remote_transfer"` | 序列化并通过网络连接发送 |

### `supported_events` 值

`"start"`、`"end"`、`"fail"`、`"data_ready"`、`"timeout"`

---

## 解析器自动生成指南

每个叶字段遵循 `WorkflowParser` 识别的四种模式之一：

```cpp
// 字符串
config.field = JsonHelper::get_string(obj, "field", "default");

// 整数（根据需要强制转换为 int）
config.field = static_cast<int>(JsonHelper::get_int(obj, "field", 0));

// 布尔值
config.field = JsonHelper::get_bool(obj, "field", true);

// 字符串数组
auto arr = JsonHelper::get_array(obj, "field");
if (arr) {
    for (size_t i = 0; i < arr->array_size(); ++i) {
        auto v = arr->array_at(i);
        if (v && v->is_string()) config.field.push_back(v->as_string());
    }
}
```

使用 `publish/workflow/workflow_sample.jsonc` 的代码生成器可以：

1. 遍历 `nodes[0]` 和 `edges[0]` 对象中的每个 JSON 键。
2. 从值类型推断辅助调用：`string` → `get_string`、`number` → `get_int` 或 `get_double`、`bool` → `get_bool`、`array` → `get_array` + 元素循环。
3. 为每个键发出一个 C++ 语句，将字段名和示例值作为默认参数插入。
4. 对于类似枚举的字符串字段（例如 `execution_mode`、`transfer_mode`），发出 if/else-if 链将每个记录的字符串映射到相应的 C++ 枚举值。

---

## 验证规则

- `workflow.id` 必须非空。
- `node.id` 必须非空且在工作流内唯一。
- `edge.id` 必须非空且在工作流内唯一。
- `edge.source` 和 `edge.target` 必须引用现有的节点 `id` 值。
- 默认按 DAG 校验；当 edge 使用 `loop/goto` 控制时，允许显式环回并进入控制流校验路径。
