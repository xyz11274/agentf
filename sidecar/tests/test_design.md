# Sidecar 测试设计

> 聚焦：数据格式不匹配 + 多模态返回处理不当 + 请求/响应对齐 SysAgent

## 1. 业务约束表

| 约束ID | 业务规则 | 违反后的业务影响 |
|--------|---------|----------------|
| B1 | HandleUserRequest 只接受 TaskAssignment / UserInput，其他返回 400 | 非 task 消息被错误当作用户输入转发 Bridge |
| B2 | 异步路径只写 WriteStatueCode(200)，不写 body | CMF 框架 ASYNC 语义混乱 |
| B3 | 同步路径只写 WriteStatueCode(200/500)，不写 body | 同上 |
| B4 | UserInputText + UserInputObject 合并为 OpenAI messages 格式 | 多模态内容丢失 |
| B5 | 回调 JSON Header 透传字段（AcmSessionId、DeviceId 等）原样回传 | 上游丢失 session/device 上下文 |
| B6 | SSE 流中 `_source != "coordinator"` 的消息不回调上游 | skill_executor 中间过程泄漏给用户 |
| B7 | SSE `[DONE]` 信号终止流读取 | 流不终止 → 超时 → 线程泄漏 |
| B8 | 回调 POST 发送 UserOutput (Running) + Completed + TaskStatus 三阶段 | 上游收不到任务完成信号 |
| B9 | Sender/Receiver 在回调中翻转 | 回调消息方向反了 |
| **B10** | **UserInputObject 按 string 解析（CMF 协议是 stringified JSON），不直接当嵌套对象** | **多模态输入静默丢失** |
| **B11** | **回调 UserOutput 必须包含 UserOutputObject 字段（string 类型）** | **多模态输出（图片/卡片）丢失** |
| **B12** | **TaskStatusCode 类型与 MessageType 匹配：Report=string, Management=int** | **上游解析类型错误** |
| **B13** | **回调 UserOutput 包含 IsVoiceBroadcast / ImplicitOutputFlag / UserManualInterventionFlag** | **上游默认行为异常** |

## 2. 系统不变量

| ID | 不变量 |
|----|--------|
| I1 | HandleUserRequest 返回前不阻塞 |
| I2 | 回调 target URL = `https://<sys_agent_entity>:23767/coremind/systemagent/v1/callback` |
| I3 | 回调 JSON 顶层只有 Header 和 Data |
| I4 | 每个 SSE token 一次 Running 回调；流结束一次 Completed + 一次 TaskStatus |
| I5 | request_id = `sidecar-<递增数字>` |
| I6 | HandleHealthCheck 返回 `{"status":"healthy","coremind":true,"sidecar":true}` |

## 3. 数据格式不匹配问题汇总

### 问题 1：UserInputObject 类型错误（输入侧）

SysAgent `TaskAssignment::Parse()`:
```
JsonParser::GetString(root, "UserInputObject", userInputObject);  // → string
```

Sidecar 当前:
```
data["UserInputObject"]["data"]["content"]  // → 当嵌套 JSON 对象访问
```

CMF 协议中 `UserInputObject` 是 **stringified JSON string**。直接 `["data"]` 访问 string 值得到 null → 静默丢弃多模态输入。

### 问题 2：回调缺少 UserOutputObject（输出侧）

SysAgent `UserOutput::ToJson()`:
```
jsonObj["UserOutputObject"] = userOutputObject;    // string
jsonObj["UserManualInterventionFlag"] = ...;
jsonObj["ImplicitOutputFlag"] = ...;
jsonObj["IsVoiceBroadcast"] = ...;
```

Sidecar post_callback 缺少：UserOutputObject、UserManualInterventionFlag、ImplicitOutputFlag、IsVoiceBroadcast。

### 问题 3：TaskStatusCode 类型混用

SysAgent 有两个结构体：
- `TaskStatusReport`: `string taskStatusCode`，无 UserOutputText
- `TaskStatusManagement`: `int taskStatusCode`，有 UserOutputText

Sidecar 用 MessageType="TaskStatusReport" + TaskStatusCode=200(int) + UserOutputText → 混用两个结构体。

### 问题 4：缺少业务上下文字段

Sidecar 未提取：RewriteQ、LongHistory、ToolName、UserIntent、UserSpecifiedAgent。

### 问题 5：Header 必选字段未校验

SysAgent 强制校验 9 个必选字段，sidecar 全部 `.get("field", "")` 静默默认。

## 4. 错误模型

| 错误模型 | 约束 | 测试目标 |
|---------|------|---------|
| EventType 未校验 | B1 | TaskStop/DataExchange 返回 400 |
| 异步路径误写 body | B2 | 验证只调 WriteStatueCode |
| UserInputObject 当嵌套 JSON 访问 | B10 | string 输入时正确二次解析 |
| 回调缺 UserOutputObject | B11 | 回调 JSON 包含该字段 |
| TaskStatusCode int/string 混用 | B12 | 类型与 MessageType 一致 |
| UserInputObject 路径不存在 | B4 | 无 UserInputObject 不崩溃 |
| 透传字段遗漏 | B5 | Header 有 AcmSessionId 时回调包含 |
| coordinator 过滤被删除 | B6 | `_source=skill_executor` 不回调 |
| SSE [DONE] 未处理 | B7 | 收到 [DONE] 终止 |
| Sender/Receiver 未翻转 | B9 | 回调 Sender = 原 Receiver |

## 5. 场景组合表

| 场景 | 输入组合 | 约束 | 错误模型 | 优先级 |
|------|---------|------|---------|--------|
| TaskAssignment 正常 | UserTaskManagement + TaskAssignment + UserInputText | B1,B2 | EventType 未校验 | P0 |
| UserInput 正常 | DataExchange + UserInput | B1 | EventType 未校验 | P0 |
| 非 task 消息被拒 | TaskStatusManagement | B1 | 非 task 消息被处理 | P0 |
| 异步路径无 body | sys_agent_entity 非空 | B2,I1 | 异步写 body | P0 |
| **UserInputObject = stringified JSON** | `"{\"data\":{\"content\":[{image}]}}"` | **B10** | **嵌套 JSON 访问 null** | **P0** |
| **UserInputObject = 空 string** | `""` | **B10** | **空值崩溃** | **P0** |
| **UserInputObject = 非 JSON string** | `"plain text"` | **B10** | **parse 崩溃** | **P1** |
| **回调包含 UserOutputObject** | SSE 含 artifacts | **B11** | **多模态输出丢失** | **P0** |
| **TaskStatusCode 类型正确** | Report=string | **B12** | **类型混用** | **P0** |
| 多模态输入 | UserInputText + UserInputObject(image) | B4,B10 | 多模态丢失 | P1 |
| 回调透传字段 | Header 含 AcmSessionId | B5 | 透传遗漏 | P1 |
| Sender/Receiver 翻转 | Sender=APP, Receiver=SystemAgent | B9 | 方向反 | P1 |
| coordinator 过滤 | `_source=skill_executor` | B6 | 中间过程泄漏 | P1 |
| SSE [DONE] | payload=[DONE] | B7 | 流不终止 | P1 |
| 空 body | req_body="" | — | 空指针 | P2 |
| 非法 JSON | "not json" | — | 解析崩溃 | P2 |
| 健康检查 | GET /health | I6 | 格式错误 | P2 |

## 6. TC ID 列表

### 数据格式 + 多模态（重点）

| TC ID | 场景 | 约束 | 类型 |
|-------|------|------|------|
| TC-SID-020 | UserInputObject = stringified JSON → 正确提取 image | B10 | 单元 |
| TC-SID-021 | UserInputObject = 空 string → 不崩溃，只有 text | B10 | 单元 |
| TC-SID-022 | UserInputObject = 非 JSON string → 不崩溃 | B10 | 单元 |
| TC-SID-023 | 回调 UserOutput 包含 UserOutputObject 字段 | B11 | 单元 |
| TC-SID-024 | 回调 TaskStatusCode 类型与 MessageType 匹配 | B12 | 单元 |
| TC-SID-025 | 回调 UserOutput 包含 IsVoiceBroadcast 等 | B13 | 单元 |
| TC-SID-026 | SSE 含 artifacts → UserOutputObject 承载 artifacts | B11 | 单元 |
| TC-SID-027 | 完整多模态流：text+image 输入 → text+image 输出 | B10,B11 | 集成 |

### 请求/响应对齐

| TC ID | 场景 | 约束 | 类型 |
|-------|------|------|------|
| TC-SID-001 | TaskAssignment → 异步路径 | B1,B2,I1 | 单元 |
| TC-SID-002 | UserInput → 异步路径 | B1 | 单元 |
| TC-SID-003 | TaskStop → 400 | B1 | 单元 |
| TC-SID-004 | DataExchange → 400 | B1 | 单元 |
| TC-SID-005 | 空 body → 400 | — | 单元 |
| TC-SID-006 | 非法 JSON → 400 | — | 单元 |
| TC-SID-007 | UserInputText + Image 多模态合并 | B4 | 单元 |
| TC-SID-008 | 只有 UserInputText | B4 | 单元 |
| TC-SID-009 | 异步路径只写 StatusCode | B2,I1 | 单元 |
| TC-SID-010 | 同步路径只写 StatusCode | B3 | 单元 |
| TC-SID-011 | 回调透传 AcmSessionId/DeviceId | B5 | 单元 |
| TC-SID-012 | 回调 Sender/Receiver 翻转 | B9 | 单元 |
| TC-SID-013 | SSE `_source=skill_executor` 过滤 | B6 | 单元 |
| TC-SID-014 | SSE `[DONE]` 终止 | B7 | 单元 |
| TC-SID-015 | 健康检查格式 | I6 | 单元 |
| TC-SID-016 | 同步 E2E：sidecar → mock Bridge → 200 | B3 | 集成 |
| TC-SID-017 | 异步 E2E：sidecar → mock Bridge SSE → 回调 | B8,I4 | 集成 |

## 7. 全局测试目录

| TC ID | 约束 | 自动化位置 | 状态 |
|-------|------|-----------|------|
| TC-SID-001 | B1,B2,I1 | `tests/test_router_logic.cpp::TestEventType::task_assignment` | 待实现 |
| TC-SID-002 | B1 | `tests/test_router_logic.cpp::TestEventType::user_input` | 待实现 |
| TC-SID-003 | B1 | `tests/test_router_logic.cpp::TestEventType::task_stop_rejected` | 待实现 |
| TC-SID-004 | B1 | `tests/test_router_logic.cpp::TestEventType::data_exchange_rejected` | 待实现 |
| TC-SID-005 | — | `tests/test_router_logic.cpp::TestParsing::empty_body` | 待实现 |
| TC-SID-006 | — | `tests/test_router_logic.cpp::TestParsing::invalid_json` | 待实现 |
| TC-SID-007 | B4 | `tests/test_router_logic.cpp::TestContent::multimodal_merge` | 待实现 |
| TC-SID-008 | B4 | `tests/test_router_logic.cpp::TestContent::text_only` | 待实现 |
| TC-SID-009 | B2,I1 | `tests/test_router_logic.cpp::TestResponse::async_no_body` | 待实现 |
| TC-SID-010 | B3 | `tests/test_router_logic.cpp::TestResponse::sync_no_body` | 待实现 |
| TC-SID-011 | B5 | `tests/test_router_logic.cpp::TestCallback::passthrough_fields` | 待实现 |
| TC-SID-012 | B9 | `tests/test_router_logic.cpp::TestCallback::sender_receiver_swap` | 待实现 |
| TC-SID-013 | B6 | `tests/test_router_logic.cpp::TestSSE::skill_executor_filtered` | 待实现 |
| TC-SID-014 | B7 | `tests/test_router_logic.cpp::TestSSE::done_terminates` | 待实现 |
| TC-SID-015 | I6 | `tests/test_router_logic.cpp::TestHealth::format` | 待实现 |
| **TC-SID-020** | **B10** | **`tests/test_router_logic.cpp::TestInputFormat::stringified_json`** | **待实现** |
| **TC-SID-021** | **B10** | **`tests/test_router_logic.cpp::TestInputFormat::empty_string`** | **待实现** |
| **TC-SID-022** | **B10** | **`tests/test_router_logic.cpp::TestInputFormat::non_json_string`** | **待实现** |
| **TC-SID-023** | **B11** | **`tests/test_router_logic.cpp::TestOutputFormat::has_output_object`** | **待实现** |
| **TC-SID-024** | **B12** | **`tests/test_router_logic.cpp::TestOutputFormat::status_code_type`** | **待实现** |
| **TC-SID-025** | **B13** | **`tests/test_router_logic.cpp::TestOutputFormat::has_optional_fields`** | **待实现** |
| **TC-SID-026** | **B11** | **`tests/test_router_logic.cpp::TestOutputFormat::artifacts_to_output_object`** | **待实现** |
| **TC-SID-027** | **B10,B11** | **`tests/test_async_e2e.py::test_multimodal_flow`** | **待实现** |
| TC-SID-016 | B3 | `tests/test_sync_e2e.py::test_sync_bridge_200` | 待实现 |
| TC-SID-017 | B8,I4 | `tests/test_async_e2e.py::test_async_sse_callback` | 待实现 |

## 8. 测试架构

### Layer 1 — 纯逻辑单元测试（C++ GoogleTest）

提取 `sidecar_router.cpp` 中无 CMF 依赖的纯逻辑到 `router_logic.h`：

```
sidecar/
├── router_logic.h       ← 纯逻辑声明
├── router_logic.cpp     ← 纯逻辑实现
├── sidecar_router.cpp   ← 调用 router_logic
└── tests/
    ├── test_design.md        ← 本文档
    └── test_router_logic.cpp ← GoogleTest
```

提取的函数：
- `bool is_supported_message_type(header_msg_type, data_msg_type)` → B1
- `Json::Value build_openai_messages(user_input_text, user_input_object_str)` → B4,B10
- `Json::Value build_callback_json(task_id, dialogue_id, ...)` → B5,B9,B11,B13
- `std::optional<std::string> extract_sse_token(sse_line)` → B6,B7
- `std::string build_task_status_json(...)` → B12

### Layer 2 — Python 集成测试

- `test_sync_e2e.py` — stub 模式 httplib 路径
- `test_async_e2e.py` — mock Bridge SSE + 回调验证

### Layer 3 — CI 生产模式

COREMIND_ENABLED=ON 环境下测试完整 CMF Context 交互。
