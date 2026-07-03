# MMNode 开发规范

## 适用范围
本文档定义 `publish/workflow` 下自定义 MMNode 的通用开发约定。
说明：MMNode 在 DSL 中定义执行行为，运行时由 Worker 执行引擎执行多语言代码。

**详细开发指南请参考**: [NODE_DEVELOPMENT_GUIDE.md](NODE_DEVELOPMENT_GUIDE.md) - 完整的 Node 开发指南，包含各执行模式的具体实现方法和场景化示例。

## 1. 通用入口契约

所有 MMNode 实现建议遵循：
- 输入：来自 runtime/databus 的 JSON 载荷
- 输出：写回 JSON（stdout 或输出文件）
- 退出码：`0` 表示成功，非 `0` 表示失败

推荐 Shell 入口：

```bash
node_xxx.sh --input <input.json> --output <output.json> [--context <context.json>]
```

推荐 Python 入口：

```bash
python node_xxx.py --input <input.json> --output <output.json>
```

## 2. 最小输入输出结构

输入 JSON：
- `trace_id`（字符串）
- `workflow_id`（字符串）
- `node_id`（字符串）
- `payload`（对象）

输出 JSON：
- `status`（`SUCCESS` / `FAILED`）
- `payload`（对象）
- `error`（可选，字符串）

## 3. 与 MMWorkflow 的绑定

workflow node 字段建议至少包含：
- `execution_mode`
- `command`
- `inputs` / `outputs`
- 超时与重试控制字段

## 4. 错误处理约定

- 硬失败返回非 0 退出码
- 输出结构化 `error` 字段
- stderr 保持可读，便于排障
