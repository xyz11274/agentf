# Sandbox 与 Artifact 模块改动

## 1. 模块职责

Sandbox 为 Tool 提供隔离的命令、文件和网络执行环境；Artifact Pipeline 将 Sandbox 中本轮真正生成的文件安全地回收到 Agent package，再交给 Bridge。

主文件：

- packages/agentflow-runtime/src/agentflow_runtime/common/sandbox_patch.py
- packages/agentflow-runtime/src/agentflow_runtime/executors/http.py
- packages/agentflow-runtime/src/agentflow_runtime/executors/host.py
- packages/agentflow-runtime/src/agentflow_runtime/executors/e2b.py
- packages/agentflow-runtime/src/agentflow_runtime/artifact_pipeline.py
- docker/sandbox/http_server/sandbox_http_server.py
- src/sandbox/sandbox_local.cpp
- src/sandbox/e2b_lifecycle.py
- src/sandbox/e2b_sync.py
- frontend/bridge/services/sandbox_observer.py

## 2. Sandbox endpoint 路由

最初本地 Sandbox 环境变量可能指回 AgentFlow 自身接口，Tool 再次调用自身形成递归。当前：

- Local Sandbox 为每个实例解析真实 HTTP endpoint。
- 多实例使用各自端口，不能因为共享环境变量串到另一个 Sandbox。
- Host、HTTP 和 E2B Executor 都消费明确的执行端点。
- sandbox-mode=none 时走 Host 路径，不伪造一个不可用 Sandbox endpoint。

## 3. HTTP Tool 协议补齐

Sandbox HTTP Server 与 Runtime Executor 对齐了实际协议：

- bash/exec。
- read_file、write_file、edit_file。
- list_dir。
- web_fetch。
- 文件上传和下载。

请求字段统一使用 tool_params 和标准 opts。write_file 成功结果返回明确 path，失败时返回真实错误，不再出现“HTTP 200 但文件根本没写入”的假成功。

web_fetch 支持 Sandbox 出站请求并正确解码 gzip。网络错误作为 Tool 业务结果返回，不被吞成空字符串。

## 4. 路径边界

路径解析集中到 Sandbox 语义：

- 用户相对路径映射到当前 Conversation 工作区。
- 拒绝通过 ..、绝对宿主路径或特殊名称逃出允许根目录。
- read/write/edit/list 对同一路径采用一致规则。
- 输入上传区和生成输出区分开。
- 二进制 Artifact 回收使用受限接口，不通过任意宿主文件读取。

这既修复错误路径，也防止 Tool 把宿主敏感文件当作普通输入。

## 5. Artifact 来源收紧

Artifact Pipeline 原先可能从最终文本、Tool stdout、网页正文或错误信息中的路径字符串猜测产物。当前只采信结构化事实：

- Tool Result 中明确的 artifact/path 字段。
- Hook 明确追加的 Artifact。
- 本轮新观测到的生成路径。

不会把用户上传文件、旧轮次文件、网页中出现的路径或最终回答中的示例路径当作新 Artifact。

## 6. Artifact 回收和持久化

当前链路为：

1. Tool 返回 Sandbox 内结构化路径。
2. Artifact Pipeline 校验后缀、路径和本轮来源。
3. 从 Sandbox 通过受限 read/download 接口取回。
4. 放入 Agent package 或当前 User/Conversation 工作区。
5. 计算内容身份并记录 persist 终态。
6. Bridge 生成受保护的外部下载能力链接。

同批外部 Artifact 重名时不会静默覆盖；空 Artifact 字段也不会遮蔽后续非空字段。

## 7. 多媒体产物

图片和视频生成脚本的具体 Provider、模型及 Endpoint 调整不在本总结范围。项目层真正保留的变化是：

- 媒体生成使用结构化 Artifact 返回。
- 媒体落入当前 User/Conversation 工作区。
- 需要凭据的媒体调用可以在可信进程完成，再将结果安全发布到 Sandbox/Workspace。
- Skill 在目标媒体已成功产生后可以收敛，避免继续无效 Loop。

## 8. 生命周期

- 复用 Sandbox 时刷新最近活跃时间，避免正在使用的实例被 GC。
- 活跃 Dialogue 不因 Session TTL 被回收。
- Bridge 超时或客户端断开会取消底层 Dialogue，并让 Sandbox 工作停止。
- E2B 同步和模板管理使用与 Runtime 一致的路径及协议。

## 9. 形成的不变量

- 每个 Sandbox 实例只能使用自己的 endpoint。
- Tool 成功必须与实际文件或命令结果一致。
- Artifact 必须来自结构化、本轮、可验证的来源。
- 输入文件与生成产物必须分开。
- Sandbox 路径不能越过 Conversation 工作区和允许根目录。
- 网络失败不能被伪装成成功，也不能无限重试。

对应变更主要见 CHG-001、015、020、054、055、065、080、081、082、083、093、094、097、098、099、100、102、104、105、106。

