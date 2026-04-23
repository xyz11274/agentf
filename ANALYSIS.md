# AgentFlow C++ Repository - Comprehensive Analysis

> **合并说明（2026-04-23）**：与本文件主题（DataBus、SO 节点 API、MMNode/Worker 执行链等）直接相关的结论已并入 [`docs/enhancements/034-feature-agentflow-双集成与跨节点databus适配层设计-260423-tbd.md`](docs/enhancements/034-feature-agentflow-双集成与跨节点databus适配层设计-260423-tbd.md) **§13 附录 A**。下文保留英文原文便于对照，新增设计请以 enhancement 为准。

## 1. REFACTORING.md Structure Analysis

**File Status**: The old `docs/REFACTORING.md` (5427 lines) has been moved to `/docs/enhancements/` directory structure with individual enhancement files.

### H2/H3 Section Headings (from grep analysis):
The document has 100+ sections covering:
- **Node Output Data Conflict Fix** (lines 3-678) - Edge data mapping strategy
- **Workflow Input Dialogue Isolation** (lines 683-740)
- **Agent/Runtime Configuration Model** (lines 744-799)
- **Agent/Runtime Double-Queue Architecture** (lines 775-798)
- **Examples Directory Restructuring** (lines 801-847)
- **Multi-Session Chat Enhancements** (lines 851-957)
- **Multi-Language Node Refactoring** (lines 960-1065)
- **MMNode Execution Engine** (lines 1066-1101)
- **Worker Hot-Update Capability** (lines 1088-1102)
- **Runtime Master Self-Scheduling** (lines 1102-1124)
- **Visualizer GOTO Loop Visualization** (lines 1125-1135)
- **Worker/MMWorkflow/MMNode/MMEdge Self-Execution** (lines 1136-1161)
- **Vendor Dependencies** (lines 1162-1172)
- **Documentation Governance** (lines 1173-1186)
- **MMDataBus Global Sharing** (lines 1187-1198)
- **Directory Structure Overview** (lines 1209-1233)
- **Optimization Content** (lines 1236-1330+)

**Key Finding**: The document is now organized as individual enhancement files in `/docs/enhancements/` directory (27 files total), each with status (implemented/tbd).

---

## 2. Public Headers for DataBus, Workflow, Node

### 2.1 External Headers (`include/external/agentflow/`)

**so_node.h** (336 lines) - **PRIMARY EXTERNAL API** for SO library nodes:
- `SoDataVariant` - Tagged union: `std::variant<monostate, bool, int64_t, double, std::string>`
- `SoDataMap` - `std::map<std::string, SoDataVariant>`
- **V1 Signature**: `void node(const SoDataMap& inputs, SoDataMap& outputs)`
- **V2 Signature**: `void node(const SoDataMap& inputs, SoDataMap& outputs, INodeDataBus* databus)`
- **V3 Signature**: `void node(const SoDataMap& inputs, SoDataMap& outputs, INodeDataBusClient* client)` ← **RECOMMENDED**

**INodeDataBus (V2) Interface** (lines 77-129):
```cpp
class INodeDataBus {
  virtual bool put(const std::string& key, const SoDataVariant& value) = 0;
  virtual bool get(const std::string& key, SoDataVariant& value) const = 0;
  virtual bool contains(const std::string& key) const = 0;
  virtual bool remove(const std::string& key) = 0;
  virtual bool publish(const std::string& topic, const SoDataVariant& value) = 0;
  virtual SubscriptionHandle subscribe(const std::string& key, SoDataBusCallback callback) = 0;
  virtual SubscriptionHandle subscribe_topic(const std::string& topic, SoDataBusCallback callback) = 0;
  virtual void unsubscribe(const SubscriptionHandle& handle) = 0;
  virtual void unsubscribe_all() = 0;
  virtual const char* kv_dir() const = 0;
  virtual const char* events_dir() const = 0;
};
```

**INodeDataBusClient (V3) Interface** (lines 229-260) - **Session-Isolated Access**:
```cpp
class INodeDataBusClient {
  virtual INamespaceClient* sys() = 0;     // Framework config & response
  virtual INamespaceClient* ctx() = 0;     // Context storage
  virtual INamespaceClient* llm() = 0;     // LLM interaction
  virtual INamespaceClient* tool() = 0;    // Tool calls
  virtual INamespaceClient* user() = 0;    // User-defined data
  virtual const std::string& session_id() const = 0;
  virtual INodeDataBus* raw() = 0;         // Raw access (bypasses isolation)
};
```

**INamespaceClient** (lines 180-205):
```cpp
class INamespaceClient {
  virtual bool put(const std::string& key, const SoDataVariant& value) = 0;
  virtual bool get(const std::string& key, SoDataVariant& value) const = 0;
  virtual bool contains(const std::string& key) const = 0;
  virtual bool remove(const std::string& key) = 0;
  virtual SubscriptionHandle subscribe(const std::string& key, SoDataBusCallback callback) = 0;
  virtual void unsubscribe(const SubscriptionHandle& handle) = 0;
};
```
**Key Prefix Format**: `/session/{session-id}/{namespace}/{key}`

**Helper Functions** (lines 275-331):
- `to_string(SoDataVariant)` - Convert variant to string
- `get_string(SoDataMap, key, default)` - Extract string with fallback
- `get_int(SoDataMap, key, default)` - Extract int64_t
- `get_double(SoDataMap, key, default)` - Extract double
- `get_bool(SoDataMap, key, default)` - Extract bool
- `has_key(SoDataMap, key)` - Check existence

### 2.2 Internal Headers (`include/internal/workflow/`)

**node.h** (305 lines) - MMNode Core Class:
```cpp
class MMNode {
  enum class ExecutionMode {
    CPP_FUNCTION, SHELL_SCRIPT, EXECUTABLE, 
    GO_SCRIPT, PYTHON_SCRIPT, JAVA_JAR, SO_LIBRARY
  };

  struct Config {
    NodeId id, name, type, priority;
    std::vector<FieldDefinition> input_definitions, output_definitions;
    ExecutionMode execution_mode;
    std::string command, working_dir;
    int max_retries, timeout_ms, memory_limit_mb, cpu_limit;
    std::function<bool(const DataMap& inputs, DataMap& outputs)> cpp_function;
    bool json_output, auto_start_runtime;
    std::string workflow_ref, node_context_workflow;
  };

  // Core APIs
  bool initialize(), execute(), cleanup();
  
  // DataBus injection
  void set_databus(runtime::MMDataBus* databus);
  void set_databus_client(std::shared_ptr<INodeDataBusClient> client);
  
  // Plugin UDS for external communication
  void set_plugin_uds_path(const std::string& path);
  
  // Runtime queue + event loop
  bool start_runtime(const RuntimeConfig& config);
  bool enqueue_message(const Message& message);
  bool enqueue_event(const NodeEvent& event);
  
  // Hot reload
  bool enable_code_reload(), reload_code(), check_and_reload_code();
  
  // Execution strategies
  bool execute_cpp_function();
  bool execute_shell_command();
  bool execute_with_worker_engine();  // ← Delegates to MMWorkerEngine
  bool execute_with_timeout(int timeout_ms, std::function<bool()> executor);
  bool execute_so_library();
  bool execute_jar_file();
  bool execute_script(runtime::ExecutableType type);
};
```

---

## 3. MMDataBus Structure and API

### 3.1 Core MMDataBus Class (`include/internal/runtime/databus/databus.h`)

```cpp
class MMDataBus {
  struct OwnerContext {
    std::string request_id, user_id, client_id, agent_id, session_id, 
                task_id, workflow_id, node_id, master_id, worker_id;
  };

  struct DataRef {
    std::string data_id, address, checksum;
  };

  // Core K-V operations
  bool put(const std::string& key, const DataVariant& value);
  bool get(const std::string& key, DataVariant& value) const;
  bool remove(const std::string& key);
  bool contains(const std::string& key) const;

  // Prefix operations
  int remove_by_prefix(const std::string& prefix);
  std::vector<std::string> list_keys_by_prefix(const std::string& prefix) const;

  // Subscription/Pub-Sub (Push model)
  void subscribe(const std::string& key, const DataChangeCallback& callback);
  void unsubscribe(const std::string& key);

  // Data reference operations (for large payloads)
  bool store_with_ref(const std::string& key, const DataVariant& value, 
                      const OwnerContext& owner, DataRef& out_ref);
  bool resolve_ref(const std::string& data_ref, DataVariant& value) const;
  std::vector<std::string> find_refs_by_owner(const std::string& owner_key, 
                                              const std::string& owner_value) const;

  // Remote connection
  bool connect_remote(const std::string& address);
  void disconnect_remote();

  // Component access
  std::shared_ptr<LocalDatabus> local() const;
  std::shared_ptr<RemoteDatabus> remote() const;

  // Statistics
  size_t get_used_capacity() const;
  size_t get_total_capacity() const;
  int get_data_count() const;
};
```

### 3.2 LocalDataBus (`include/internal/runtime/databus/local_databus.h`)

**LocalChannel** (Message Queue):
- Send/receive DataPackets with metadata (key, value, source_id, timestamp, sequence#)
- Event subscription with callbacks
- Timeout-aware blocking receive

**ZeroCopyDataBus** (Shared Memory):
```cpp
class ZeroCopyDataBus {
  bool put(const std::string& key, const DataVariant& value);
  bool get(const std::string& key, DataVariant& value) const;
  bool remove(const std::string& key);
  int remove_by_prefix(const std::string& prefix);
  std::vector<std::string> list_keys_by_prefix(const std::string& prefix) const;
  
  // Pointer-based access (zero-copy)
  const DataVariant* get_data_ptr(const std::string& key) const;
  bool get_data_ref(const std::string& key, DataVariant& ref) const;
  
  // Subscription/Pub-Sub
  void subscribe(const std::string& key, const DataChangeCallback& callback);
  void unsubscribe(const std::string& key);
  
  // Thread-safe access
  void lock_read() const, unlock_read() const;
  void lock_write(), unlock_write();
};
```

**SharedMemoryFactory**:
- `create_in_process()` - In-process shared memory
- `create_posix_shared()` - POSIX IPC shared memory
- `create_systemv_shared()` - System V IPC
- `create_best()` - Auto-select best available

### 3.3 RemoteDatabus (`include/internal/runtime/databus/remote_databus.h`)

```cpp
class RemoteDatabus {
  bool connect(const std::string& address);
  void disconnect();
  bool is_connected() const;

  // Remote operations
  bool publish(const std::string& key, const DataVariant& value);
  bool fetch(const std::string& key, DataVariant& value) const;
  bool remove(const std::string& key);

  // Subscription
  void subscribe(const std::string& key, DataChangeCallback callback);
  void unsubscribe(const std::string& key);

  // Ownership indexing (for multi-response tracking)
  bool index_data_ref(const std::string& owner_index_key, const std::string& data_ref);
  std::vector<std::string> find_indexed_refs(const std::string& owner_index_key) const;
};
```

### 3.4 Push/Subscription Support

**YES - MMDataBus Supports Push/Subscription Patterns**:

1. **Key-based subscriptions**: `subscribe(key, callback)` triggers when key data changes
2. **Topic-based subscriptions** (V2): `subscribe_topic(topic, callback)`
3. **Callback signature**: `std::function<void(const std::string& key, const DataVariant& value)>`
4. **Multi-response tracking**: `index_data_ref()` + `find_indexed_refs()` for distributed response correlation
5. **Cross-process**: Remote databus maintains subscriptions across network boundaries

---

## 4. MMNode External Execution Flow

### 4.1 Execution Delegation to Worker Engine

**From node.cpp:523-590** - `execute_with_worker_engine()`:

```cpp
bool MMNode::execute_with_worker_engine() {
  // Build execution request
  runtime::MMWorkerEngine::ExecutionRequest request;
  request.command = config_.command;
  request.function_name = config_.function_name;
  request.main_class = config_.main_class;
  request.working_dir = config_.working_dir;
  request.args = config_.args;
  request.timeout_ms = (config_.timeout_ms > 0) ? config_.timeout_ms : 30000;
  request.inputs = inputs_;
  
  // Pass DataBus for SO library V2 signature
  request.databus = databus_;
  
  // Pass session_id for V3 signature
  if (databus_client_) {
    request.session_id = databus_client_->session_id();
  }
  
  // Plugin UDS path for external plugin communication
  request.plugin_uds_path = plugin_uds_path_;
  
  // Execution mode mapping
  switch(config_.execution_mode) {
    case ExecutionMode::SHELL_SCRIPT:
      request.mode = MMWorkerEngine::ExecutionMode::SHELL_SCRIPT;
      break;
    case ExecutionMode::EXECUTABLE:
      request.mode = MMWorkerEngine::ExecutionMode::EXECUTABLE;
      break;
    case ExecutionMode::GO_SCRIPT:
      request.mode = MMWorkerEngine::ExecutionMode::GO_SCRIPT;
      break;
    case ExecutionMode::PYTHON_SCRIPT:
      request.mode = MMWorkerEngine::ExecutionMode::PYTHON_SCRIPT;
      break;
    case ExecutionMode::JAVA_JAR:
      request.mode = MMWorkerEngine::ExecutionMode::JAVA_JAR;
      break;
    case ExecutionMode::SO_LIBRARY:
      request.mode = MMWorkerEngine::ExecutionMode::SO_LIBRARY;
      break;
  }
  
  // Execute via worker engine
  auto response = runtime::MMWorkerEngine::execute_code(request);
  
  // Apply outputs
  outputs_ = response.outputs;
  if(config_.json_output && !response.stdout_content.empty()) {
    DataMap parsed;
    if(parse_flat_json_object(response.stdout_content, parsed)) {
      for(auto& kv : parsed) {
        outputs_[kv.first] = std::move(kv.second);
      }
    }
  }
  
  return response.success;
}
```

### 4.2 MMWorkerEngine API (`include/internal/runtime/worker.h`)

```cpp
class MMWorkerEngine {
  enum class ExecutionMode {
    CPP_FUNCTION, SHELL_SCRIPT, EXECUTABLE, 
    GO_SCRIPT, PYTHON_SCRIPT, JAVA_JAR, SO_LIBRARY
  };

  struct ExecutionRequest {
    ExecutionMode mode;
    std::string command, function_name, main_class, working_dir;
    std::vector<std::string> args;
    int timeout_ms = 30000;
    DataMap inputs;
    std::string databus_root_dir;
    bool keep_databus_files = false;
    bool enable_hot_reload = false;
    HotReloadConfig hot_reload_config;
    MMDataBus* databus = nullptr;        // For V2 signature
    std::string session_id;               // For V3 signature
    std::string plugin_uds_path;          // For external plugin UDS
    LLMServiceConfig llm_config;
  };

  struct ExecutionResponse {
    bool success = false;
    DataMap outputs;
    std::string stdout_content, stderr_content, error_message;
    int exit_code = -1;
    std::string active_version_id;
  };

  static ExecutionResponse execute_code(const ExecutionRequest& request);
  static bool check_and_reload_code(const std::string& command, const HotReloadConfig& config);
  static std::shared_ptr<ExecutableVersion> get_code_version(const std::string& command);
  static bool rollback_code_version(const std::string& command, const std::string& version_id);
};
```

### 4.3 Execution Sequence

1. **Node.execute()** → checks state, calls appropriate executor
2. **execute_with_worker_engine()** → marshals MMWorkerEngine::ExecutionRequest
3. **MMWorkerEngine::execute_code()** → dispatches to appropriate handler:
   - **SO_LIBRARY**: Load .so, call V1/V2/V3 signature function
   - **EXECUTABLE/SHELL_SCRIPT/PYTHON_SCRIPT/GO_SCRIPT/JAVA_JAR**: Spawn subprocess
4. **Worker injects environment**:
   - `AF_OUTPUT_FORMAT=json` (if node->json_output)
   - `AF_DATABUS_UDS_FILE` (UDS socket path for DataBus access)
   - `AF_SESSION_ID` (session ID for V3 isolation)
   - `AF_SELF_TASK_ID` (task ID for message routing)
   - `AF_PLUGIN_UDS_FILE` (plugin communication socket)
   - `AF_SELF_NODE_ID`, `AF_SELF_WORKFLOW_ID`, `AF_SELF_AGENT_ID`

---

## 5. Adapters in vendor/adapter/

**Directory Structure** (`vendor/adapter/`):
- `actor/` - Actor model adapters (ProtoActor, stub implementations)
  - `adapter_factory.cpp` - Factory for actor selection
  - `message_serializer.cpp` - Message serialization
  - `protoactor_impl.cpp` - ProtoActor implementation
  - `stub_impl.cpp` - Stub/mock implementation
- `cache/` - Caching adapters
  - `redis_adapter.cpp` - Redis cache backend
- `json/` - JSON parsing/serialization adapters
  - `json_adapter_factory.cpp` - JSON adapter factory
  - `json_adapter_rapidjson.cpp` - RapidJSON implementation
  - `json_adapter_stub.cpp` - Stub JSON adapter
- `network/` - **EMPTY** (no network adapters currently)
- `serialization/` - Serialization adapters
- `storage/` - Storage adapters

**Key Finding**: **NO existing HTTP, REST, or gRPC adapters**. Network directory is empty.

---

## 6. Streaming/SSE/Chunked Response Handling

### 6.1 Existing Streaming Support

**StreamingLLMClient** (`include/internal/runtime/llm/streaming_client.h`):
```cpp
class StreamingLLMClient {
  struct Config {
    std::string base_url, api_key, model = "gpt-4.1";
    int timeout_ms = 30000;
  };

  using ChunkCallback = std::function<void(const std::string&)>;
  using FinishCallback = std::function<void(const std::string&)>;
  using ErrorCallback = std::function<void(const std::string&)>;

  bool stream_prompt(const std::string& prompt, const Config& config,
                     const ChunkCallback& on_chunk,
                     const FinishCallback& on_finish = FinishCallback(),
                     const ErrorCallback& on_error = ErrorCallback()) const;
};
```

**Implementation** (`src/runtime/llm/streaming_client.cpp`):
- Supports mock:// protocol for testing
- Builds curl SSE streaming command with `curl -sS -N` (streaming without buffering)
- Parses SSE `data:` format line-by-line
- Extracts `"content"` field from JSON chunks
- Handles `[DONE]` marker
- Callbacks: `on_chunk()` per message, `on_finish()` with full text, `on_error()`

**Key Finding**: **Basic SSE streaming exists but limited**:
- Only curl-based HTTP
- Reads from pipe, no non-blocking I/O
- No multiplexing/concurrent streams
- Works with mock:// and OpenAI-compatible APIs

### 6.2 Multi-Response Mechanism

**From enhancements doc**: `020-bugfix-node-输出数据冲突问题修复-260318-tbd.md` + `022-feature-multi-response-机制设计-260319-implemented.md`

DataBus supports multi-response correlation via:
- `index_data_ref(owner_index_key, data_ref)` - Index response by owner (request_id, user_id, task_id, etc.)
- `find_indexed_refs(owner_index_key)` - Retrieve all responses for owner
- Key pattern: `/response/{node_id}/{request_id}/{response_index}`

---

## 7. Current MMDataBus Read/Write API

### 7.1 Synchronous Operations
```cpp
// Put - write with local + optional remote sync
bool put(const std::string& key, const DataVariant& value);

// Get - read from local, fallback to remote with auto-backfill
bool get(const std::string& key, DataVariant& value) const;

// Remove
bool remove(const std::string& key);

// Check existence
bool contains(const std::string& key) const;
```

### 7.2 Prefix Operations
```cpp
int remove_by_prefix(const std::string& prefix);
std::vector<std::string> list_keys_by_prefix(const std::string& prefix) const;
```

### 7.3 Pub-Sub/Push Patterns ✓
```cpp
// Key subscription (push on write)
void subscribe(const std::string& key, const DataChangeCallback& callback);
void unsubscribe(const std::string& key);

// Callback fires when key data changes:
using DataChangeCallback = std::function<void(const std::string&, const DataVariant&)>;
```

### 7.4 Large Data (Reference) Pattern
```cpp
// Store with reference generation (for large payloads)
bool store_with_ref(const std::string& key, const DataVariant& value, 
                    const OwnerContext& owner, DataRef& out_ref);
// Returns: DataRef{data_id, address="databus://global/<data_id>", checksum}

// Resolve reference
bool resolve_ref(const std::string& data_ref, DataVariant& value) const;

// Multi-response correlation
bool index_data_ref(const std::string& owner_index_key, const std::string& data_ref);
std::vector<std::string> find_indexed_refs(const std::string& owner_index_key) const;
```

### 7.5 Architecture: Read-Through Cache with Remote Sync
```
Put: Local → Remote (if connected)
Get: Local → (miss) → Remote + backfill Local
Subscribe: Local + Remote (if connected)
```

---

## 8. Example: LLM Query Agent (`examples/agent/llm_query_agent/`)

**LLM Node Implementation** (`nodes/llm_node.cpp`) - **V3 Signature Example**:

```cpp
extern "C" void llm_query_node_v3(const DataMap& inputs, DataMap& outputs, 
                                   INodeDataBusClient* client) {
  // Session info
  if (client) {
    std::cerr << "[LLM_NODE_V3] Session ID: " << client->session_id() << std::endl;
  }

  // Read config from sys namespace (framework-injected)
  std::string api_key, model, base_url;
  if (client && client->sys()) {
    SoDataVariant value;
    if (client->sys()->get("api_key", value)) {
      api_key = std::get<std::string>(value);
    }
    if (client->sys()->get("model", value)) {
      model = std::get<std::string>(value);
    }
    if (client->sys()->get("base_url", value)) {
      base_url = std::get<std::string>(value);
    }
  }

  // Fallback to inputs
  if (model.empty()) {
    model = get_string_from_map(inputs, "model", "gpt-4.1");
  }
  if (base_url.empty()) {
    base_url = get_string_from_map(inputs, "base_url");
  }

  // Get prompt
  std::string prompt = get_string_from_map(inputs, "prompt");
  if (prompt.empty()) {
    prompt = get_string_from_map(inputs, "input");
  }

  // Validate
  if (api_key.empty()) {
    outputs["status"] = std::string("error");
    outputs["error"] = std::string("API key not provided");
    return;
  }
  if (prompt.empty()) {
    outputs["status"] = std::string("error");
    outputs["error"] = std::string("No prompt provided");
    return;
  }

  // Call LLM
  try {
    std::string response = llm_query::simple_query(api_key, prompt, model, base_url);
    outputs["status"] = std::string("success");
    outputs["response"] = response;
  } catch (const std::exception& e) {
    outputs["status"] = std::string("error");
    outputs["error"] = std::string(e.what());
  }
}
```

**Workflow Files**:
- `llm_query_workflow.json` - Defines dispatcher → llm_query → coordinator pipeline
- `dispatcher_node.sh` - Parses user input
- `coordinator_node.sh` - Aggregates results

---

## 9. LLM Service Configuration

**From MMWorkerEngine::ExecutionRequest**:
```cpp
struct LLMServiceConfig {
  std::string base_url;      // e.g., "https://api.openai.com/v1"
  std::string api_key;       // Bearer token
  std::string model = "gpt-4.1";
  int timeout_ms = 30000;
};
```

**Injection Mechanism**:
- Passed via ExecutionRequest from MMNode
- Available to SO libraries via INodeDataBusClient->sys()->get("api_key")
- Also injected as environment variables for script nodes

---

## 10. Key Architectural Points for Planning

### 10.1 External API Invocation Pattern
1. **Node defines execution mode** (ExecutionMode enum)
2. **Marshals inputs** to DataMap or JSON
3. **Delegates to MMWorkerEngine** with ExecutionRequest
4. **Worker spawns process or loads SO**
5. **Subprocess/SO communicates via**:
   - **DataBus UDS socket** (AF_DATABUS_UDS_FILE)
   - **Plugin UDS socket** (AF_PLUGIN_UDS_FILE)
   - **Environment variables** (AF_SESSION_ID, AF_SELF_TASK_ID, etc.)
6. **Returns ExecutionResponse** with outputs

### 10.2 DataBus Architecture (Hybrid)
- **Local**: ZeroCopyDataBus (in-process shared memory, POSIX/SysV support)
- **Remote**: RemoteDatabus (network sync, reference tracking)
- **Pub-Sub**: Both support subscription callbacks
- **Multi-Response**: Indexing by owner context for correlation

### 10.3 Streaming/Chunked Responses
- **Existing**: StreamingLLMClient with curl SSE parsing
- **Gap**: No general-purpose HTTP streaming adapter
- **Opportunity**: Build pluggable HTTP client adapter in vendor/network/

### 10.4 Session Isolation (V3 DataBus)
- **Key format**: `/session/{session-id}/{namespace}/{key}`
- **Namespaces**: SYS, CTX, LLM, TOOL, USER
- **Per-node**: Set via request.session_id in ExecutionRequest

### 10.5 Plugin UDS Mechanism
- **Purpose**: External processes can connect back to master via UDS socket
- **Path**: AF_PLUGIN_UDS_FILE environment variable
- **Use case**: Script nodes needing bidirectional communication

---

## 11. Document Splitting Recommendations

The old REFACTORING.md should be split into organized documentation:

**Proposed Structure**:
1. `docs/architecture/` - Architectural decisions
2. `docs/enhancements/` - Individual enhancement tracking (already done)
3. `docs/guides/` - Developer guides (execution patterns, node development)
4. `docs/api/` - API reference (DataBus, MMNode, Worker)
5. `docs/examples/` - Annotated examples

---

## Summary Table

| Component | Key Class | Primary File | API Type |
|-----------|-----------|--------------|----------|
| Public Node API | INodeDataBus (V2), INodeDataBusClient (V3) | include/external/agentflow/so_node.h | Virtual interfaces |
| DataBus (Unified) | MMDataBus | include/internal/runtime/databus/databus.h | Sync K-V + Pub-Sub |
| Local Storage | ZeroCopyDataBus | include/internal/runtime/databus/local_databus.h | In-process shared mem |
| Remote Storage | RemoteDatabus | include/internal/runtime/databus/remote_databus.h | Network sync |
| Node Execution | MMNode | include/internal/workflow/node.h | Control plane |
| Worker Engine | MMWorkerEngine | include/internal/runtime/worker.h | Execution dispatch |
| LLM Streaming | StreamingLLMClient | include/internal/runtime/llm/streaming_client.h | Curl-based SSE |
| Adapters | (factory) | vendor/adapter/{actor,cache,json,serialization,storage}/ | Plugin architecture |

