#pragma once

#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>
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
//                     "DeviceId", "SessionId", "SysAgentEntity",
//                     "TeamEnvironmentID", "UUID"}
// receiver_override 非空时覆盖翻转后的 Receiver（对齐 AISF 协议 §2.2：
// TaskStatusManagement 报文 Header.Receiver 固定为 "SystemAgent"，
// 与 UserOutput（Receiver=请求 Sender）不同）。
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
    const std::string& receiver_override = "");

// sidecar_interface_contract.md §2-3: EventTimeStamp 需 ISO 8601 格式。
// 返回 "2026-07-03T09:42:51.123Z" 形式字符串（毫秒精度）。
std::string current_timestamp_iso8601();

// stderr/stdout 日志前缀时间戳——本地时间 + 可读格式 + 毫秒精度，
// 如 "2026-08-03 12:34:05.123"。供 cerr/cout 输出加前缀，便于从日志时间序定位事件。
std::string log_timestamp();

// TC-SID-026: 将 SSE _artifacts 数组转换为 UserOutputObject 的 stringified JSON。
// artifacts 为空或 null 时返回 ""（用于 Running 回调）。
// artifacts 非空时返回 {"code":200101,"data":{"content":[...]}} 的字符串形式。
std::string build_user_output_object(const Json::Value& sse_artifacts);

// ── 产物 ACM 上传编排（L4 §4.2） ──────────────────────────────────────
// 遍历 user_output_object["data"]["content"]，对每条产物（ArtifactInfo）：
//   ① 内容获取（url 优先 → base64 兜底，用户拍板 2026-08-14）：
//      fetch_fn(url) 下载；空/失败 → 读 data_base64 经 acm_base64_decode 解码
//   ② upload_fn(name, content, mime) 上传 ACM，返回 file_uuid（空串 = 失败）
//   ③ 成功 → 写 item["acm_file_id"]；失败 → 保留原 url（降级），不阻断
// 日志：ACM_ART_DL / ACM_ART_B64 / ACM_ART_UP_OK / ACM_ART_UP_ERR / ACM_ART_SKIP。
// 返回：成功注入 acm_file_id 的条数。
// fetch_fn/upload_fn 由调用方注入（sidecar_router 生产 / main.cpp dev），
// 本函数保持纯编排可测（不含网络副作用）。
int enrich_artifacts_with_acm(
    Json::Value& user_output_object,
    const std::function<std::string(const std::string& url)>& fetch_fn,
    const std::function<std::string(const std::string& name,
                                    const std::string& content,
                                    const std::string& mime)>& upload_fn);

// F7 展示层截断：递归遍历 JSON，将 base64 data URI（如 "data:image/png;base64,xxx"）
// 截断为"前缀 + 前 120 字符 + 长度标记"，防止超长 base64 刷屏日志。
// 仅用于日志展示——存储层（dumper 落盘）仍保留完整数据。
// 返回 toStyledString 格式的截断结果（main.cpp / sidecar_router.cpp 的 pretty_json 共用）。
std::string json_to_styled_truncated(const Json::Value& v);

// 字符串级 base64 截断（不解析 JSON，直接找 "base64," 标记截断）——
// 用于 SSE_RAW 等打印原始行的日志点（main.cpp / sidecar_router.cpp），
// 保留原始行其余部分不变；超长 base64 载荷截为前 120 字符 + full_len 标记。
std::string truncate_base64_in_text(const std::string& raw);

// ── SSE chunk → UserOutput 回调决策器（dev/prod 共享） ──────────────
// 封装 chunk 类型判断、flag 决策、final_text/artifacts 状态维护。
// main.cpp 和 sidecar_router.cpp 都调用此类，确保行为一致。
class SSEChunkProcessor {
public:
    struct Callback {
        bool should_send = false;
        std::string stream_status = "Running";
        int flag = 0;
        std::string text;
        bool is_final = false;
        std::string user_output_object;
    };

    // 处理一条 SSE data JSON，返回回调指令（should_send=false 表示跳过）。
    // 内部维护 accumulated_content_ / final_text_ / accumulated_artifacts_ / error_message_。
    // 检测 bridge SSE 错误信封 {"error":{"message":"...","code":-32000}}（chat_rest.py:1004-1021）：
    // 命中时设置 error_message_，调用方应据此跳过 Completed 回调、改走错误 TaskStatus 分支。
    std::vector<Callback> process(const Json::Value& sse_json);

    // 流结束后构造 Completed 回调（携带 final_text + artifacts）。
    // 注意：has_error()=true 时不应调用此方法——上游应改发错误 TaskStatus。
    Callback make_completed();

    // bridge SSE 流内是否收到 error payload。true 表示流已以错误终止，
    // 上游应跳过 Completed UserOutput，改发 TaskStatusCode=500 + BridgeError + error_message。
    bool has_error() const { return !error_message_.empty(); }
    const std::string& error_message() const { return error_message_; }

    const std::string& accumulated_content() const { return accumulated_content_; }

private:
    std::string accumulated_content_;
    std::string final_text_;
    Json::Value accumulated_artifacts_;
    std::string error_message_;  // bridge SSE error payload 的 message，空=无错误
};

// ── SidecarDumper — 请求/响应 dump（dev/prod 共享） ────────────────
// 环境变量 AF_SIDECAR_DUMP_DIR 控制，空=禁用。
// 每个请求生成一个 JSONL 文件：{timestamp}_{request_id}.jsonl
class SidecarDumper {
public:
    SidecarDumper(const std::string& request_id);
    ~SidecarDumper();

    void dump_request(const Json::Value& body);
    void dump_sse_chunk(const Json::Value& sse_json);
    void dump_callback(const std::string& stream_status, int flag,
                       const std::string& text, bool is_final,
                       const std::string& user_output_object);
    // dump_callback_request：记录发往 upstream 的完整 HTTP 请求体（F7/F7.1 铁律——
    // 与外部系统交互的原始请求必须完整落盘，供联合 debug）。callback_type 区分
    // "user_output"（流式回调）vs "task_status"（TaskStatusReport）；target 为完整 URL；
    // body 为构造好的 JSON 请求体原文（cb_str/ts_str）。
    void dump_callback_request(const std::string& callback_type,
                              const std::string& target,
                              const std::string& body);
    // dump_callback_result：记录发往 upstream 回调的结果。error 仅存错误详情
    // （如 "null response" / httplib 连接错误），response 存上游返回的完整响应体
    // （F7.1 铁律——外部响应必须完整落盘，成功/失败都要）。via 区分 "coremind_h2"/"httplib"。
    void dump_callback_result(const std::string& callback_type,
                              const std::string& via, int http_status,
                              const std::string& error,
                              const std::string& response);

private:
    std::ofstream file_;
    std::ofstream callback_file_;  // callback 类数据单独文件，便于和上游沟通
    bool enabled_ = false;
};

}
