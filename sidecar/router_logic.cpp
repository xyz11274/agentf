#include "router_logic.h"
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
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

std::optional<Json::Value> parse_request_body(const std::string& body) {
    if (body.empty()) return std::nullopt;

    Json::Value root;
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream input(body);
    if (!Json::parseFromStream(reader, input, &root, &errors)) {
        return std::nullopt;
    }
    return root;
}

Json::Value build_health_response() {
    Json::Value body;
    body["status"] = "healthy";
    body["coremind"] = true;
    body["sidecar"] = true;
    return body;
}

Json::Value build_callback_header(
    const std::string& message_type,
    const std::string& task_id,
    const std::string& dialogue_id,
    const std::string& user_name,
    const std::string& client_session_id,
    const std::string& original_sender,
    const std::string& original_receiver,
    const std::string& seq_num,
    const Json::Value& req_header) {
    auto [new_sender, new_receiver] = swap_sender_receiver(original_sender, original_receiver);

    Json::Value header;
    header["MessageType"] = message_type;
    header["TaskID"] = task_id;
    header["DialogueId"] = dialogue_id;
    header["UserName"] = user_name;
    header["ClientSessionId"] = client_session_id;
    header["Sender"] = new_sender;
    header["Receiver"] = new_receiver;
    header["SeqNum"] = seq_num;

    for (const auto& k : {"AcmSessionId", "ClientExField", "DeviceCategory",
                          "DeviceId", "SessionId", "TeamEnvironmentID", "UUID"}) {
        if (req_header.isMember(k)) header[k] = req_header[k];
    }
    return header;
}

std::string build_user_output_object(const Json::Value& sse_artifacts) {
    if (!sse_artifacts.isArray() || sse_artifacts.empty()) return "";

    Json::Value output;
    output["code"] = 200101;
    output["data"]["content"] = sse_artifacts;

    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    w["emitUTF8"] = true;
    return Json::writeString(w, output);
}

// ── SSEChunkProcessor ──────────────────────────────────────────────

std::vector<SSEChunkProcessor::Callback> SSEChunkProcessor::process(const Json::Value& sse_json) {
    std::vector<Callback> results;

    Json::Value chunk_artifacts = sse_json.get("_artifacts", sse_json.get("artifacts", Json::Value(Json::nullValue)));
    if (!chunk_artifacts.isNull()) {
        accumulated_artifacts_ = chunk_artifacts;
    }

    std::string token;
    std::string reasoning;
    auto choices = sse_json["choices"];
    if (choices.isArray() && choices.size() > 0) {
        auto delta = choices[0]["delta"];
        if (delta.isObject()) {
            auto rc = delta["reasoning_content"];
            if (rc.isString()) reasoning = rc.asString();
            auto dc = delta["content"];
            if (dc.isString()) token = dc.asString();
        } else {
            auto msg = choices[0]["message"]["content"];
            if (msg.isString()) token = msg.asString();
        }
    }

    if (token.empty() && reasoning.empty()) return results;

    std::string _src = sse_json.get("_source", "").asString();
    if (!_src.empty() && _src != "coordinator") return results;

    if (!reasoning.empty()) {
        results.push_back({true, "Running", 1, reasoning, false, ""});
    }

    std::string _chunk_type = sse_json.get("_chunk_type", "").asString();
    std::string _skill = sse_json.get("_skill", "").asString();
    bool is_final = sse_json.get("_final", false).asBool();

    if (_chunk_type == "llm_chunk") {
        if (!token.empty()) {
            results.push_back({true, "Running", 0, token, false, ""});
            accumulated_content_ += token;
        }
        return results;
    }

    if (is_final) {
        if (!token.empty()) final_text_ = token;
        return results;
    }

    if (!_skill.empty() && !token.empty()) {
        results.push_back({true, "Running", 1, token, false, ""});
    }

    return results;
}

SSEChunkProcessor::Callback SSEChunkProcessor::make_completed() {
    Callback cb;
    cb.should_send = true;
    cb.stream_status = "Completed";
    cb.flag = 0;
    cb.text = final_text_;
    cb.is_final = true;
    cb.user_output_object = build_user_output_object(accumulated_artifacts_);
    return cb;
}

// ── SidecarDumper ──────────────────────────────────────────────────

static std::string dump_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

SidecarDumper::SidecarDumper(const std::string& request_id) {
    const char* dir = std::getenv("AF_SIDECAR_DUMP_DIR");
    if (!dir || !*dir) return;

    std::filesystem::create_directories(dir);
    std::string filename = std::string(dir) + "/" + dump_timestamp() + "_" + request_id + ".jsonl";
    file_.open(filename);
    if (file_.is_open()) {
        enabled_ = true;
        std::cerr << "[sidecar_dumper] dump file: " << filename << std::endl;
    } else {
        std::cerr << "[sidecar_dumper] WARN: cannot open " << filename << std::endl;
    }
}

SidecarDumper::~SidecarDumper() {
    if (file_.is_open()) file_.close();
}

void SidecarDumper::dump_request(const Json::Value& body) {
    if (!enabled_) return;
    Json::Value entry;
    entry["_meta"] = true;
    entry["_timestamp"] = dump_timestamp();
    entry["request"] = body;
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    w["emitUTF8"] = true;
    file_ << Json::writeString(w, entry) << "\n";
    file_.flush();
}

void SidecarDumper::dump_sse_chunk(const Json::Value& sse_json) {
    if (!enabled_) return;
    Json::Value entry = sse_json;
    entry["_direction"] = "sse_in";
    entry["_timestamp"] = dump_timestamp();
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    w["emitUTF8"] = true;
    file_ << Json::writeString(w, entry) << "\n";
    file_.flush();
}

void SidecarDumper::dump_callback(const std::string& stream_status, int flag,
                                  const std::string& text, bool is_final,
                                  const std::string& user_output_object) {
    if (!enabled_) return;
    Json::Value entry;
    entry["_direction"] = "callback_out";
    entry["_timestamp"] = dump_timestamp();
    entry["stream_status"] = stream_status;
    entry["flag"] = flag;
    entry["text"] = text;
    entry["is_final"] = is_final;
    entry["user_output_object"] = user_output_object;
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    w["emitUTF8"] = true;
    file_ << Json::writeString(w, entry) << "\n";
    file_.flush();
}

void SidecarDumper::dump_callback_result(const std::string& callback_type,
                                          const std::string& via, int http_status,
                                          const std::string& error) {
    if (!enabled_) return;
    Json::Value entry;
    entry["_direction"] = "callback_result";
    entry["_timestamp"] = dump_timestamp();
    entry["callback_type"] = callback_type;
    entry["via"] = via;
    entry["http_status"] = http_status;
    if (!error.empty()) {
        entry["error"] = error;
    }
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    w["emitUTF8"] = true;
    file_ << Json::writeString(w, entry) << "\n";
    file_.flush();
}

}
