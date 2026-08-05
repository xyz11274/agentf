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
    // coordinator / skill_executor 均可回调 upstream(后者仅 progress 汇总文本,
    // 细分过滤在 SSEChunkProcessor::process 内完成)
    return source == "coordinator" || source == "skill_executor";
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
    const Json::Value& req_header,
    const std::string& receiver_override) {
    auto [new_sender, new_receiver] = swap_sender_receiver(original_sender, original_receiver);

    Json::Value header;
    header["MessageType"] = message_type;
    header["TaskID"] = task_id;
    header["DialogueId"] = dialogue_id;
    header["UserName"] = user_name;
    header["ClientSessionId"] = client_session_id;
    header["Sender"] = new_sender;
    // TaskStatusManagement 报文 Receiver 固定为 SystemAgent（AISF 协议 §2.2），
    // 与 UserOutput（Receiver=请求 Sender）不同；其余报文保持翻转语义。
    header["Receiver"] = receiver_override.empty() ? new_receiver : receiver_override;
    header["SeqNum"] = seq_num;

    for (const auto& k : {"AcmSessionId", "ClientExField", "DeviceCategory",
                           "DeviceId", "SessionId", "SysAgentEntity",
                           "TeamEnvironmentID", "UUID"}) {
        if (req_header.isMember(k)) header[k] = req_header[k];
    }
    return header;
}

std::string current_timestamp_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time_t_now, &tm);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count() << 'Z';
    return oss.str();
}

std::string log_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time_t_now, &tm);  // 本地时间，stderr 日志可读性优先
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
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

// F7 展示层截断——base64 data URI（如 "data:image/png;base64,xxx"）递归截断。
// 仅用于日志展示：超长 base64 会让日志不可读；存储层（dumper 落盘）保留完整数据。
namespace {
void truncate_base64_in_value(Json::Value& v) {
    if (v.isObject()) {
        for (auto& key : v.getMemberNames()) {
            truncate_base64_in_value(v[key]);
        }
    } else if (v.isArray()) {
        for (auto& item : v) {
            truncate_base64_in_value(item);
        }
    } else if (v.isString()) {
        const std::string& s = v.asString();
        // data URI 形式: "data:image/png;base64,<...>" —— 在 "base64," 后截断
        const std::string marker = "base64,";
        auto pos = s.find(marker);
        if (pos != std::string::npos && s.size() - (pos + marker.size()) > 120) {
            std::string head = s.substr(0, pos + marker.size());
            std::string tail = s.substr(pos + marker.size());
            v = head + tail.substr(0, 120)
                + "...[base64 truncated, full_len=" + std::to_string(tail.size()) + "]";
        }
    }
}

// 字符串型 JSON body 的 base64 截断:解析→截断→compact 序列化。
// 解析失败返回原串(保留现场,不丢数据)——用户要求 dump trace 亦截断 base64。
std::string truncate_base64_in_json_string(const std::string& raw) {
    if (raw.empty()) return raw;
    Json::CharReaderBuilder reader;
    Json::Value val;
    std::string errors;
    std::istringstream input(raw);
    if (!Json::parseFromStream(reader, input, &val, &errors)) {
        return raw;
    }
    truncate_base64_in_value(val);
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    w["emitUTF8"] = true;
    return Json::writeString(w, val);
}
}  // namespace

std::string json_to_styled_truncated(const Json::Value& v) {
    Json::Value copy = v;
    truncate_base64_in_value(copy);
    // emitUTF8=true:toStyledString() 默认把非 ASCII 转义为 \uXXXX,控制台日志不可读;
    // 这里用 StreamWriterBuilder 输出原生 UTF-8,并保持 tab 缩进的 pretty 格式。
    Json::StreamWriterBuilder w;
    w["indentation"] = "\t";
    w["emitUTF8"] = true;
    return Json::writeString(w, copy);
}

std::string truncate_base64_in_text(const std::string& raw) {
    const std::string marker = "base64,";
    auto pos = raw.find(marker);
    if (pos != std::string::npos && raw.size() - (pos + marker.size()) > 120) {
        std::string head = raw.substr(0, pos + marker.size());
        std::string tail = raw.substr(pos + marker.size());
        return head + tail.substr(0, 120)
            + "...[base64 truncated, full_len=" + std::to_string(tail.size()) + "]";
    }
    return raw;
}

// ── SSEChunkProcessor ──────────────────────────────────────────────

std::vector<SSEChunkProcessor::Callback> SSEChunkProcessor::process(const Json::Value& sse_json) {
    std::vector<Callback> results;

    // ── bridge SSE 错误信封检测（chat_rest.py:1004-1021）──────────────
    // bridge 在 daemon 通知中途失败/timeout 时，HTTP 200 + SSE 推 error payload：
    //   data: {"error":{"message":"...","code":-32000}}
    //   data: [DONE]
    // 命中时记录 error_message_，返回空 results（不再处理 chunk）。
    // 调用方据此跳过 Completed 回调、改发错误 TaskStatus（F1 数据安全：禁止掩盖问题）。
    if (!error_message_.empty()) return results;  // 已错则丢弃后续 chunk
    const Json::Value& err_obj = sse_json["error"];
    if (err_obj.isObject()) {
        std::string msg = err_obj.get("message", "").asString();
        if (!msg.empty()) {
            error_message_ = msg;
            std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_processor] SSE_ERROR msg=" << msg << std::endl;
        }
        return results;  // 错误 payload 不含 choices，自然走不到下面
    }

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
    // skill_executor: 仅转发 progress 汇总文本(思考过程 flag=1),
    // llm_chunk / reasoning / _final 仍不外露(coremind-bridge.md §3.3 决策)
    if (_src == "skill_executor") {
        // skill_executor: progress 汇总文本(产生端已带 \n)与 LLM 逐 token 增量(llm_chunk)
        // 均转发为思考过程(flag=1);不累积进 accumulated_content_,避免污染最终回复文本。
        // LLM 增量保持原样(含其自身换行符),由消费端按 token 组装。
        std::string _ct = sse_json.get("_chunk_type", "").asString();
        if ((_ct != "progress" && _ct != "llm_chunk") || token.empty()) return results;
        results.push_back({true, "Running", 1, token, false, ""});
        return results;
    }
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
    // callback 类数据（callback_out/callback_request/callback_result）单独落一个文件，
    // 便于和上游联合 debug 时直接拿到完整的 callback 流，不用从混合 JSONL 里筛行。
    std::string cb_filename = std::string(dir) + "/" + dump_timestamp() + "_" + request_id + "_callback.jsonl";
    callback_file_.open(cb_filename);
    if (file_.is_open() && callback_file_.is_open()) {
        enabled_ = true;
        std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_dumper] dump file: " << filename
                  << " | callback file: " << cb_filename << std::endl;
    } else {
        std::cerr << "[" << sidecar::log_timestamp() << "] [sidecar_dumper] WARN: cannot open " << filename
                  << " or " << cb_filename << std::endl;
    }
}

SidecarDumper::~SidecarDumper() {
    if (file_.is_open()) file_.close();
    if (callback_file_.is_open()) callback_file_.close();
}

void SidecarDumper::dump_request(const Json::Value& body) {
    if (!enabled_) return;
    Json::Value entry;
    entry["_meta"] = true;
    entry["_timestamp"] = dump_timestamp();
    // 用户要求 dump trace 亦截断 base64(否则落盘文件同样刷屏不可读);
    // 保留 full_len 标记,调试仍可知原始长度。
    Json::Value req = body;
    truncate_base64_in_value(req);
    entry["request"] = req;
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    w["emitUTF8"] = true;
    const std::string line = Json::writeString(w, entry) + "\n";
    file_ << line;
    callback_file_ << line;
    file_.flush();
    callback_file_.flush();
}

void SidecarDumper::dump_sse_chunk(const Json::Value& sse_json) {
    if (!enabled_) return;
    Json::Value entry = sse_json;
    truncate_base64_in_value(entry);  // 同 dump_request:base64 截断落盘(full_len 标记保留)
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
    entry["user_output_object"] = truncate_base64_in_json_string(user_output_object);
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    w["emitUTF8"] = true;
    const std::string line = Json::writeString(w, entry) + "\n";
    file_ << line;
    callback_file_ << line;
    file_.flush();
    callback_file_.flush();
}

void SidecarDumper::dump_callback_request(const std::string& callback_type,
                                          const std::string& target,
                                          const std::string& body) {
    if (!enabled_) return;
    Json::Value entry;
    entry["_direction"] = "callback_request";
    entry["_timestamp"] = dump_timestamp();
    entry["callback_type"] = callback_type;
    entry["target"] = target;
    // body 是构造好的 JSON 请求体原文（cb_str/ts_str）——解析后截断 base64 再落盘，
    // 避免超长 base64 撑爆 dump 文件（用户要求 dump trace 亦截断）；解析失败返回原串
    // 保留现场不丢数据（F7/F7.1：原始请求完整落盘语义由失败回退兜底）。
    entry["body"] = truncate_base64_in_json_string(body);
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    w["emitUTF8"] = true;
    const std::string line = Json::writeString(w, entry) + "\n";
    file_ << line;
    callback_file_ << line;
    file_.flush();
    callback_file_.flush();
}

void SidecarDumper::dump_callback_result(const std::string& callback_type,
                                          const std::string& via, int http_status,
                                          const std::string& error,
                                          const std::string& response) {
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
    if (!response.empty()) {
        entry["response"] = response;
    }
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    w["emitUTF8"] = true;
    const std::string line = Json::writeString(w, entry) + "\n";
    file_ << line;
    callback_file_ << line;
    file_.flush();
    callback_file_.flush();
}

}  // namespace sidecar
