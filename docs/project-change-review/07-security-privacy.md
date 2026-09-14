# 安全与隐私模块改动

## 1. 模块目标

安全改动不再只识别某一种模型服务的密钥变量，而是建立厂商无关的凭据注册、隔离、阻断、脱敏和提交检查。具体密钥值、Provider 配置和已经发生过的本地替换不属于本文内容。

主文件：

- packages/agentflow-runtime/src/agentflow_runtime/security/secrets.py
- packages/agentflow-runtime/src/agentflow_runtime/security/trusted_media.py
- frontend/bridge/services/secret_safety.py
- packages/agentflow-runtime/src/agentflow_runtime/tool_registry.py
- packages/agentflow-runtime/src/agentflow_runtime/harness/mcp/mcp_bridge.py
- packages/agentflow-runtime/src/agentflow_runtime/recorder.py
- scripts/quality/scan_secrets.py
- .github/workflows/credential-safety.yml

## 2. 厂商无关的凭据注册

Runtime 从环境和显式配置中识别常见凭据字段，例如 key、token、secret、password、credential 等，不依赖某一家模型厂商前缀。

注册后同时识别：

- 原始值。
- 常见字符串表示。
- 结构化对象中的敏感字段名。
- 可能跨流式 chunk 被拆开的值。

注册表只用于匹配和隔离，输出使用安全标签，不把原值写入报告。

## 3. 子进程和 Sandbox 隔离

Skill LLM 进程可以在可信边界读取模型调用所需配置，但普通 Tool、MCP Server、Sandbox 命令和媒体脚本不应自动继承所有凭据。

当前通过安全子进程环境：

- 删除已注册凭据变量。
- 只传递必要的运行身份和无敏感配置。
- 对必须调用带凭据服务的媒体任务，使用 trusted_media 在可信 Runtime 侧完成请求，再发布结果文件。

这样 Tool 执行 env、打印环境变量或读 Sandbox 文件时，不会天然获得上游模型密钥。

## 4. 出站边界

Tool Registry、MCP Bridge 和相关 HTTP 执行边界会检查参数：

- 参数中包含已注册凭据时阻断调用。
- Server 或 Tool 返回值在回到 LLM、日志或轨迹前脱敏。
- 安全阻断形成结构化 security_policy_blocked 业务状态，能进入 Skill 和轨迹终态。

阻断只针对凭据泄露，不把普通业务失败升级成框架崩溃。

## 5. 全链路脱敏

脱敏覆盖：

- Recorder 的 LLM request/response/meta。
- Span 文件和 stderr 预览。
- SessionLog Header 与事件 data。
- Bridge 的 agent.execute 请求边界。
- SSE 内容、错误文本和日志。
- Uvicorn access log。
- SQLite 用户输入和助手内容。
- Artifact 元数据及安全错误报告。

Bridge 安全模块采用故障封闭：关键脱敏器不可用时，不应把原始敏感内容作为降级结果继续落盘。

## 6. 流式与重复脱敏

流式文本可能把凭据拆在多个 chunk。SecretStreamRedactor 只暂存“可能继续组成凭据”的尾部前缀，正常长文本不会因固定大窗口长期延迟。

同一数据可能在 Runtime、Bridge、日志和 SessionLog 多次脱敏。当前占位符具有幂等性，并保留嵌套 JSON 的转义结构，避免重复处理后把合法 JSON 变成无法解析的字符串。

## 7. Artifact 访问安全

Artifact 对外地址不再由 user_id、client_id 和文件名直接拼接：

- 内部路由身份使用无歧义编码。
- 外部下载要求随机能力 token。
- 本地真实路径不直接暴露给前端。
- 路径规范化阻止越界读取。

## 8. 提交门禁

新增通用 Secret Scanner 和 CI/Pre-push 门禁，扫描多家服务常见凭据形式，而不是只匹配一种 key。示例 Agent 的真实 .env 文件从版本化内容中移除，配置模板只保留变量名或占位值。

本门禁防止后续新泄露，不代表自动清理 Git 历史中已经存在的凭据。

## 9. 形成的不变量

- 新 Provider 不需要先修改一套厂商专用脱敏代码才能获得基础保护。
- Tool 和 Sandbox 默认不能继承模型服务凭据。
- 敏感参数不得通过 Tool/MCP/HTTP 出站。
- 轨迹、日志、SQLite 和 SSE 不能保存已注册凭据原值。
- 重复脱敏必须保持数据格式有效。
- 安全策略失败应被记录为业务事实，不能被伪装成成功。

对应变更主要见 CHG-041、063、064、078、095、096、101、105、106、110、113、115、119。

