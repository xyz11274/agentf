#include "router_logic.h"
#include <sstream>

namespace sidecar {

bool is_supported_message_type(const std::string& header_msg_type,
                               const std::string& data_msg_type) {
    if (header_msg_type == "UserTaskManagement" && data_msg_type == "TaskAssignment")
        return true;
    if (header_msg_type == "DataExchange" && data_msg_type == "UserInput")
        return true;
    return false;
}

Json::Value build_openai_messages(const std::string& user_input_text,
                                  const std::string& user_input_object_str) {
    Json::Value content_parts(Json::arrayValue);

    if (!user_input_text.empty()) {
        Json::Value text_part;
        text_part["type"] = "text";
        text_part["text"] = user_input_text;
        content_parts.append(text_part);
    }

    if (!user_input_object_str.empty()) {
        Json::Value user_input_obj;
        Json::CharReaderBuilder reader;
        std::string err;
        std::istringstream stream(user_input_object_str);
        if (Json::parseFromStream(reader, stream, &user_input_obj, &err)) {
            auto content_array = user_input_obj["data"]["content"];
            if (content_array.isArray()) {
                for (const auto& item : content_array) {
                    std::string item_type = item.get("type", "").asString();
                    if (item_type == "image") {
                        Json::Value image_part;
                        image_part["type"] = "image_url";
                        image_part["image_url"]["url"] = item.get("data", "").asString();
                        content_parts.append(image_part);
                    }
                }
            }
        }
    }

    Json::Value messages(Json::arrayValue);
    Json::Value user_msg;
    user_msg["role"] = "user";
    user_msg["content"] = content_parts;
    messages.append(user_msg);
    return messages;
}

Json::Value build_user_output_data(const std::string& stream_status,
                                   int flag,
                                   const std::string& text) {
    Json::Value data;
    data["MessageType"] = "UserOutput";
    data["StreamStatus"] = stream_status;
    data["UserOutputFlag"] = flag;
    data["UserOutputText"] = text;
    data["UserOutputObject"] = "";
    data["UserManualInterventionFlag"] = false;
    data["ImplicitOutputFlag"] = false;
    data["IsVoiceBroadcast"] = 1;
    return data;
}

Json::Value build_task_status_data(int status_code,
                                   const std::string& status_report,
                                   const std::string& output_text) {
    Json::Value data;
    data["MessageType"] = "TaskStatusReport";
    data["TaskStatusCode"] = status_code;
    data["TaskStatusReport"] = status_report;
    data["UserOutputText"] = output_text;
    return data;
}

SSEResult parse_sse_line(const std::string& line) {
    SSEResult result;

    if (line.empty()) return result;
    if (line.compare(0, 6, "data: ") != 0) return result;

    std::string payload = line.substr(6);
    if (payload == "[DONE]") {
        result.should_terminate = true;
        return result;
    }

    Json::Value sse_json;
    Json::CharReaderBuilder reader;
    std::string err;
    std::istringstream ss(payload);
    if (!Json::parseFromStream(reader, ss, &sse_json, &err)) return result;

    auto choices = sse_json["choices"];
    if (!choices.isArray() || choices.empty()) return result;

    auto delta = choices[0]["delta"]["content"];
    if (delta.isString()) {
        result.token = delta.asString();
    } else {
        auto msg = choices[0]["message"]["content"];
        if (msg.isString()) {
            result.token = msg.asString();
        }
    }

    if (result.token.empty()) return result;

    result.source = sse_json.get("_source", "").asString();
    result.should_process = true;
    return result;
}

bool should_forward_to_callback(const std::string& source) {
    if (source.empty()) return true;
    return source == "coordinator";
}

std::pair<std::string, std::string> swap_sender_receiver(
    const std::string& original_sender,
    const std::string& original_receiver) {
    std::string new_sender = original_receiver.empty() ? "SysAgent" : original_receiver;
    std::string new_receiver = original_sender.empty() ? "APP" : original_sender;
    return {new_sender, new_receiver};
}

}
