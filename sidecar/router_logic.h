#pragma once

#include <optional>
#include <string>
#include <utility>
#include <json/json.h>

namespace sidecar {

// B1: EventType 校验 — 只接受 TaskAssignment / UserInput
bool is_supported_message_type(const std::string& header_msg_type,
                               const std::string& data_msg_type);

// B4,B10: 构建 OpenAI messages（处理 stringified JSON 的 UserInputObject）
// user_input_object_str 是 CMF 协议中的 stringified JSON string
Json::Value build_openai_messages(const std::string& user_input_text,
                                  const std::string& user_input_object_str);

// B11,B13: 构建回调 Data.UserOutput JSON（对齐 SysAgent UserOutput::ToJson）
Json::Value build_user_output_data(const std::string& stream_status,
                                   int flag,
                                   const std::string& text);

// B12: 构建回调 Data.TaskStatus JSON（对齐 SysAgent TaskStatusManagement::ToJson）
Json::Value build_task_status_data(int status_code,
                                   const std::string& status_report,
                                   const std::string& output_text);

struct SSEResult {
    bool should_process = false;
    bool should_terminate = false;
    std::string token;
    std::string source;
};

// B6,B7: 解析 SSE 行
SSEResult parse_sse_line(const std::string& line);

// B6: coordinator 过滤
bool should_forward_to_callback(const std::string& source);

// B9: Sender/Receiver 翻转
std::pair<std::string, std::string> swap_sender_receiver(
    const std::string& original_sender,
    const std::string& original_receiver);

// TC-SID-005/006: 解析请求体为 JSON。
// 空串或非法 JSON 返回空 optional；合法 JSON 返回填充的 optional。
std::optional<Json::Value> parse_request_body(const std::string& body);

// TC-SID-015: 构建健康检查响应。
// 返回 {"status":"healthy","coremind":true,"sidecar":true}
Json::Value build_health_response();

// TC-SID-011: 构建回调 Header JSON，执行 Sender/Receiver 翻转并透传上游字段。
// passthrough_keys = {"AcmSessionId", "ClientExField", "DeviceCategory",
//                     "DeviceId", "SessionId", "TeamEnvironmentID", "UUID"}
Json::Value build_callback_header(
    const std::string& message_type,
    const std::string& task_id,
    const std::string& dialogue_id,
    const std::string& user_name,
    const std::string& client_session_id,
    const std::string& original_sender,
    const std::string& original_receiver,
    const std::string& seq_num,
    const Json::Value& req_header);

// TC-SID-026: 将 SSE _artifacts 数组转换为 UserOutputObject 的 stringified JSON。
// artifacts 为空或 null 时返回 ""（用于 Running 回调）。
// artifacts 非空时返回 {"code":200101,"data":{"content":[...]}} 的字符串形式。
std::string build_user_output_object(const Json::Value& sse_artifacts);

}
