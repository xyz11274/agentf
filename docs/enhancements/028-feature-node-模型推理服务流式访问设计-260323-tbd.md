## 2026-03-23 Node 模型推理服务流式访问设计

### 背景

AgentFlow 的 workflow node 需要访问模型推理服务（本地 vLLM/TGI）或外部模型 API（OpenAI、Anthropic 等），并支持流式输出（Streaming）。核心设计问题是：

1. **直接访问** vs **DataBus 统一中转**？
2. 流式 chunk 如何在 workflow 内传播？
3. 多个 node 并发调用同一模型时如何做 **channel 复用**？
4. **负载均衡**应在哪一层实现？

### 现状分析

#### 已实现组件

**StreamingLLMClient**（`include/internal/runtime/llm/streaming_client.h`）

```cpp
class StreamingLLMClient {
  struct Config {
    std::string base_url;   // e.g. "http://localhost:8000/v1"
    std::string api_key;
    std::string model = "gpt-4.1";
    int         timeout_ms = 30000;
  };

  using ChunkCallback  = std::function<void(const std::string&)>;
  using FinishCallback = std::function<void(const std::string&)>;
  using ErrorCallback  = std::function<void(const std::string&)>;

  bool stream_prompt(const std::string& prompt, const Config& config,
                     const ChunkCallback& on_chunk,
                     const FinishCallback& on_finish = {},
                     const ErrorCallback&  on_error  = {}) const;
};
```

实现：curl subprocess + SSE（`data:` 行解析），支持 `mock://` 协议测试。

**Worker 调用链**（`src/runtime/worker/worker.cpp:117-159`）

```
Worker 收到 ExecutionRequest（含 llm_config）
  └─ request_id = request_fields["request_id"]
  └─ channel      = "agentflow.llm.chunk." + request_id   // 可自定义
  └─ done_channel = "agentflow.llm.done."  + request_id
  └─ error_channel= "agentflow.llm.error." + request_id
  └─ StreamingLLMClient::stream_prompt(prompt, config,
       on_chunk  → file_databus.publish(channel, chunk)
       on_finish → file_databus.put("agentflow.llm.response." + request_id, content)
                   file_databus.publish(done_channel, content)
       on_error  → file_databus.put("agentflow.llm.error." + request_id, error)
                   file_databus.publish(error_channel, error)
     )
```

**脚本节点 LLM 配置注入**（`worker.cpp:57-64`）

Worker 为 Shell/Python/Go/Java 节点注入环境变量：
```bash
AF_LLM_BASE_URL=http://localhost:8000/v1
AF_LLM_API_KEY=<token>
AF_LLM_MODEL=gpt-4.1
AF_DATABUS_UDS_FILE=/tmp/agentflow_<session_id>.sock  # DataBus UDS 接入点
```

**DataBus LLM Namespace 预留**（`include/external/agentflow/so_node.h`）

```cpp
class INodeDataBusClient {
  INamespaceClient* llm();   // 预留：LLM 交互（last_prompt, last_response）
};
```

目前为预留接口，未绑定流式 chunk channel。

#### 当前缺口

| 节点类型 | LLM 配置获取 | 流式输出 | Chunk → DataBus |
|---------|------------|---------|----------------|
| SO 节点 (V3) | Worker 注入 | ✅ Worker 代理调用 | ✅ 已实现 |
| Shell 节点 | ✅ env vars | ⚠️ 需自行 curl SSE | ❌ 需手动 UDS publish |
| Python 节点 | ✅ env vars | ⚠️ 需自行 httpx/requests | ❌ 需手动 UDS publish |
| Go/Java 节点 | ✅ env vars | ⚠️ 需自行实现 | ❌ 需手动 UDS publish |

**Channel 命名规范**已在代码中实现但未文档化（见下文）。

---

### 行业实践参考

| 方案 | 代表实现 | 负载均衡层 | 流式支持 | 延迟 | 复杂度 |
|------|---------|-----------|---------|------|--------|
| 直接 HTTP/SSE | vLLM, TGI, OpenAI SDK | 服务侧 continuous batching；或 gateway（nginx/LiteLLM） | ✅ 原生 SSE | 最低 | 低 |
| 消息队列中转 | Celery+Redis, Kafka | 队列消费者侧，动态 batch | ⚠️ 需 polling 模拟 | +队列深度延迟 | 高 |
| Sidecar 代理 | Envoy, LiteLLM proxy | 代理层路由（多模型/多 provider） | ✅ 透明转发 | +1 hop | 中 |
| 共享 Actor | Ray Serve | Actor 调度器 | ✅ 支持 | 中 | 中 |

**核心结论**：
- **负载均衡不在 DataBus 层做**。业界主流将其放在服务侧（vLLM/TGI 内部 continuous batching）或透明代理层（LiteLLM/nginx），调用方只需配置 `base_url`，无感知。DataBus 层串行中转会引入不必要的瓶颈。
- **直接 HTTP/SSE 是生产主流**，vLLM/TGI 均暴露 OpenAI 兼容端点，无需中间件。
- **Channel 复用**通过 `request_id` 解决（一个请求一个 channel），不需要 pub/sub 多路复用器。

---

### 方案对比

#### 方案 A：直接访问（主推）

```
Node → Worker → StreamingLLMClient → LLM HTTP/SSE → chunks → DataBus publish
```

- **SO 节点**：Worker 代理调用，chunk 自动 publish 到 DataBus（已实现）
- **脚本节点**：读取 `AF_LLM_*` env vars，自行调用 HTTP（语言原生库），然后通过 UDS DataBus publish chunk

**负载均衡策略（按优先级）**：
1. **服务侧**（首选）：vLLM/TGI continuous batching，调用方零感知
2. **Gateway 层**（次选）：LiteLLM proxy / nginx upstream，只需改 `AF_LLM_BASE_URL`，对 AgentFlow 完全透明
3. **客户端扩展**（备选）：StreamingLLMClient.Config 扩展为 `base_urls` 列表 + round-robin，适合简单多副本场景

**性能特征**：
- 延迟 = LLM 服务延迟（无额外 hop）
- 并发 = 每个 node 独立连接，无竞争
- 资源 = 每个请求独占一个 curl subprocess（当前实现）；后续可改为 libcurl 异步

| 优势 | 劣势 |
|------|------|
| 最低延迟，零额外 hop | 脚本节点需自行实现 HTTP streaming |
| 负载均衡在服务层，不侵入框架 | chunk → DataBus 路径需手动实现（脚本节点） |
| request_id 天然隔离，无 channel 冲突 | StreamingLLMClient 当前为 curl subprocess（阻塞） |

#### 方案 B：DataBus 统一中转（特殊场景）

```
Node → DataBus request channel → LLM Proxy Plugin (Plugin UDS) → LLM API → DataBus chunk channel → Node
```

基于 enhancement 019（Plugin UDS 注入机制）实现一个 LLM Proxy Plugin 进程，统一处理所有 LLM 请求。

**适用场景（DataBus 中转有意义）**：
- 跨 session 的 GPU 动态批处理（多 session prompt 合并为一个推理批次，需 proxy 感知）
- 统一限速/配额管理（token bucket 在 proxy 层）
- 运行时模型切换/A-B 路由（proxy 决定路由哪个模型）
- 需要请求持久化或审计的场景

**接口设计**（tbd）：
```cpp
// Node V3 发起请求
client->llm()->put("request." + request_id, SoDataVariant(prompt));

// Node V3 订阅 chunk
client->llm()->subscribe("chunk." + request_id, [](const std::string& key, const SoDataVariant& chunk) {
    // 处理流式 chunk
});
```

**关键问题**：
- Proxy 需维护 request_id → session_id → subscriber 映射，本质复现 HTTP 连接管理的复杂度
- 对典型 AgentFlow 场景（单 session 单模型）引入不必要的串行瓶颈
- 与方案 A 不互斥，可作为可选 Plugin 并存

---

### 推荐设计：以直接访问为主路径

#### 1. Chunk Channel 命名规范（补文档，现有实现）

```
agentflow.llm.chunk.{request_id}    # 流式 chunk，每个 token/片段
agentflow.llm.done.{request_id}     # 完整响应（finish reason）
agentflow.llm.error.{request_id}    # 错误信息
agentflow.llm.response.{request_id} # DataBus KV 持久化（可查询）
```

`request_id` 由调用方生成（如 `{node_id}_{workflow_instance_id}_{seq}`），保证请求唯一性和 channel 隔离。Channel 名可通过 `request_fields["channel"]` 自定义覆盖（已支持）。

**多节点并发场景**：
```
Node-A: request_id=A → 订阅 agentflow.llm.chunk.A → 自己的 chunk stream
Node-B: request_id=B → 订阅 agentflow.llm.chunk.B → 自己的 chunk stream
```
天然隔离，无需额外多路复用机制。

**跨节点广播场景**（如 LLM node → streaming response node）：
```
LLM Node: publish chunk → agentflow.llm.chunk.{request_id}
Response Node: subscribe agentflow.llm.chunk.{request_id} → 实时转发给用户
```
通过 DataBus pub/sub 实现，已有基础设施支持。

#### 2. 负载均衡推荐路径

```
# 选项 1（推荐）：服务侧 continuous batching
AF_LLM_BASE_URL=http://vllm-service:8000/v1  # vLLM 内部处理并发请求

# 选项 2：透明 gateway
AF_LLM_BASE_URL=http://litellm-proxy:4000/v1  # LiteLLM 统一路由多 provider/多副本

# 选项 3（未来扩展）：客户端 round-robin
llm_config.base_urls = ["http://replica1:8000/v1", "http://replica2:8000/v1"]
```

#### 3. 各节点类型接入方式

**SO 节点（V3）**：已由 Worker 完整实现，无需修改。参考 `examples/agent/llm-query-agent/`。

**Shell 节点**：

```bash
#!/bin/bash
# 通过环境变量获取 LLM 配置（Worker 自动注入）
BASE_URL="${AF_LLM_BASE_URL}"
API_KEY="${AF_LLM_API_KEY}"
MODEL="${AF_LLM_MODEL}"
REQUEST_ID="${AF_SELF_NODE_ID}_${AF_SELF_WORKFLOW_ID}_$(date +%s%N)"

# 发起流式请求，逐 chunk 写入 DataBus
curl -sS -N \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer ${API_KEY}" \
  --data "{\"model\":\"${MODEL}\",\"stream\":true,\"messages\":[{\"role\":\"user\",\"content\":\"${PROMPT}\"}]}" \
  "${BASE_URL}/chat/completions" | while IFS= read -r line; do
    if [[ "$line" == data:* ]]; then
      payload="${line#data: }"
      [[ "$payload" == "[DONE]" ]] && break
      chunk=$(echo "$payload" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['choices'][0]['delta'].get('content',''),end='')" 2>/dev/null)
      if [[ -n "$chunk" ]]; then
        # 通过 UDS DataBus 发布 chunk（tbd：需要 agentflow-databus-cli 工具）
        echo "chunk:${chunk}" >&2  # 当前：写入 stderr，框架捕获
      fi
    fi
  done
```

**Python 节点**：

```python
import os
import httpx

base_url  = os.environ["AF_LLM_BASE_URL"]
api_key   = os.environ.get("AF_LLM_API_KEY", "")
model     = os.environ.get("AF_LLM_MODEL", "gpt-4.1")
prompt    = inputs.get("prompt", "")

chunks = []
with httpx.Client() as client:
    with client.stream(
        "POST",
        f"{base_url}/chat/completions",
        headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"},
        json={"model": model, "stream": True, "messages": [{"role": "user", "content": prompt}]},
        timeout=30,
    ) as resp:
        for line in resp.iter_lines():
            if line.startswith("data: "):
                payload = line[6:]
                if payload == "[DONE]":
                    break
                import json
                data = json.loads(payload)
                chunk = data["choices"][0]["delta"].get("content", "")
                if chunk:
                    chunks.append(chunk)
                    # tbd: publish chunk to DataBus via UDS

outputs["response"] = "".join(chunks)
```

#### 4. DataBus 中转（可选 Plugin，tbd）

基于 enhancement 019 的 Plugin UDS 机制，可实现 LLM Proxy Plugin：
- 适用场景：跨 session GPU batch、统一限速、多模型动态路由
- Plugin 进程独立运行，通过 `AF_PLUGIN_UDS_FILE` 与 Worker 通信
- 详细设计待 enhancement 029 单独立项

---

### 实施路径

#### 阶段 1：文档化（本文档，状态：tbd）

- [x] 规范化 chunk channel 命名（`agentflow.llm.{chunk|done|error|response}.{request_id}`）
- [x] 文档化 SO 节点现有调用路径
- [x] 提供 Shell/Python 参考代码
- [ ] 在 `examples/agent/llm-query-agent/` 中补充脚本节点示例

#### 阶段 2：工具化（后续）

- [ ] 提供 `agentflow-llm-stream` Shell helper 脚本，封装 curl SSE → UDS DataBus publish
- [ ] 提供 `vendor/openjiuwen/python/agentflow/llm.py` Python helper 模块
- [ ] 在 `src/runtime/llm/streaming_client.cpp` 中支持 `base_urls` 列表 + round-robin

#### 阶段 3：接口化（可选）

- [ ] 在 `INodeDataBusClient` 中正式暴露 streaming LLM 订阅 API
- [ ] 实现 DataBus LLM Proxy Plugin（见 enhancement 029）
- [ ] 将 `StreamingLLMClient` 从 curl subprocess 改为 libcurl 或 httpx 异步实现

---

### 相关文档

- [019-feature-plugin-uds-注入机制设计-260317-implemented.md](019-feature-plugin-uds-注入机制设计-260317-implemented.md) - Plugin UDS 机制（DataBus 中转 Plugin 的基础）
- [022-feature-multi-response-机制设计-260319-implemented.md](022-feature-multi-response-机制设计-260319-implemented.md) - 多响应 DataBus 索引机制
- [docs/databus_user_manual.md](../databus_user_manual.md) - DataBus pub/sub API
- [docs/UDS_INTERFACE.md](../UDS_INTERFACE.md) - UDS DataBus 接口规范
