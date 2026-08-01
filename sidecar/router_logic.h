#pragma once

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

}
