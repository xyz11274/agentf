#include "uds_model_proxy.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <json/json.h>
#include <sstream>
#include <thread>

#include <httplib.h>

#ifdef COREMIND_ENABLED
#include "resolve_handler.h"
#include <framework_com/http_client_factory.h>
#endif

// ── UDS 帧协议常量 ──
// 帧格式：4 字节大端长度 + JSON body
// 来自 frontend/mocks/mock_uds_model_server/mock_uds_server.py
static constexpr size_t FRAME_HEADER_SIZE = 4;
static constexpr size_t MAX_FRAME_SIZE = 16 * 1024 * 1024;  // 16MB

// ── IMInferScheduler 已知端点 ──
// 来自 HealthAgent IMInferServiceClient.OpenAI.cpp
static constexpr const char* IM_INFER_CHAT_PATH = "/infermind/scheduler/v1/chat/completions";
static constexpr int IM_INFER_DEFAULT_PORT = 27321;

// ── 辅助函数 ──

static std::string json_dumps(const Json::Value& val) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    w["emitUTF8"] = true;
    return Json::writeString(w, val);
}

static std::string make_uuid() {
    static std::atomic<unsigned long long> counter{0};
    auto id = ++counter;
    std::ostringstream os;
    os << "evt_" << std::hex << id;
    return os.str();
}

static std::string current_timestamp_ms() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::to_string(ms);
}

// ── 构造器／析构器 ──

UdsModelProxy::UdsModelProxy() = default;
UdsModelProxy::~UdsModelProxy() { stop(); }

// ── 启动 ──

bool UdsModelProxy::start(const std::string& socket_path, const std::string& llm_api_url,
                           bool coremind_mode) {
    if (running_) {
        std::cerr << "[uds_proxy] already running" << std::endl;
        return false;
    }

    socket_path_ = socket_path;
    llm_api_url_ = llm_api_url;
    coremind_mode_ = coremind_mode;

    // 清理旧 socket 文件
    unlink(socket_path_.c_str());

    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        std::cerr << "[uds_proxy] failed to create socket: " << strerror(errno) << std::endl;
        return false;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[uds_proxy] bind failed: " << strerror(errno) << std::endl;
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    if (listen(server_fd_, 10) < 0) {
        std::cerr << "[uds_proxy] listen failed: " << strerror(errno) << std::endl;
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    running_ = true;
    listener_thread_ = std::thread(&UdsModelProxy::run_loop, this);

    std::cout << "[uds_proxy] listening on " << socket_path_
              << " mode=" << (coremind_mode_ ? "coremind" : "httplib") << std::endl;
    return true;
}

// ── 停止 ──

void UdsModelProxy::stop() {
    if (!running_) return;
    running_ = false;
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }
    if (listener_thread_.joinable()) {
        listener_thread_.join();
    }
    unlink(socket_path_.c_str());
    std::cout << "[uds_proxy] stopped" << std::endl;
}

// ── 监听循环 ──

void UdsModelProxy::run_loop() {
    while (running_) {
        struct sockaddr_un client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (running_) {
                std::cerr << "[uds_proxy] accept failed: " << strerror(errno) << std::endl;
            }
            continue;
        }
        // 每个客户端连接在独立线程中处理
        std::thread(&UdsModelProxy::handle_client, this, client_fd).detach();
    }
}

// ── 客户端处理 ──

void UdsModelProxy::handle_client(int client_fd) {
    while (running_) {
        std::string json_body;
        if (!recv_frame(client_fd, json_body)) {
            break;
        }

        Json::Value req;
        Json::CharReaderBuilder reader;
        std::string errors;
        std::istringstream input(json_body);
        if (!Json::parseFromStream(reader, input, &req, &errors)) {
            std::cerr << "[uds_proxy] invalid JSON frame: " << errors << std::endl;
            continue;
        }

        std::string method = req.get("method", "").asString();
        std::string request_id = req.get("request_id", "").asString();
        auto params = req["params"];

        if (method == "llm.chat") {
            auto request = params["request"];
            bool stream = request.get("stream", true).asBool();
            handle_llm_chat(client_fd, request_id, json_dumps(request), stream);
        } else if (method == "heartbeat") {
            Json::Value resp;
            resp["request_id"] = request_id;
            resp["status"] = "success";
            resp["data_json"] = R"({"pong":true})";
            send_frame(client_fd, json_dumps(resp));
        } else {
            Json::Value resp;
            resp["request_id"] = request_id;
            resp["status"] = "error";
            resp["error_code"] = 404;
            resp["error_message"] = "unknown method: " + method;
            send_frame(client_fd, json_dumps(resp));
        }
    }
    close(client_fd);
}

// ── llm.chat 处理 ──

void UdsModelProxy::handle_llm_chat(int client_fd, const std::string& request_id,
                                     const std::string& body, bool stream) {
#ifdef COREMIND_ENABLED
    if (coremind_mode_) {
        // ── 生产路径：解析 IMInferScheduler 并转发 ──
        auto result = resolve_service("IMInferScheduler");
        if (!result.success || result.instances.empty()) {
            std::cerr << "[uds_proxy] IMInferScheduler resolution failed" << std::endl;
            Json::Value err_event;
            err_event["event_id"] = make_uuid();
            err_event["event_type"] = "llm_error";
            err_event["request_id"] = request_id;
            err_event["channel"] = "llm.chat";
            err_event["done"] = true;
            Json::Value err_data;
            err_data["error_code"] = 503;
            err_data["error_message"] = "IMInferScheduler unavailable";
            err_event["data"] = json_dumps(err_data);
            err_event["timestamp"] = current_timestamp_ms();
            send_frame(client_fd, json_dumps(err_event));
            return;
        }

        auto& inst = result.instances[0];
        std::string target = "https://" + inst.ip + ":" + std::to_string(inst.port);
        std::cout << "[uds_proxy] forwarding to IMInferScheduler: "
                  << target << IM_INFER_CHAT_PATH << std::endl;

        // 使用 CoreMind HTTP/2 客户端（支持 mTLS 证书）
        auto httpClientFactory = CMFrm::COM::HttpClientFactory::GetHttpClientFactory();
        auto http2Client = httpClientFactory->GetHttp2Client("default");
        if (!http2Client) {
            std::cerr << "[uds_proxy] CoreMind HTTP/2 client not available" << std::endl;
            Json::Value err_event;
            err_event["event_id"] = make_uuid();
            err_event["event_type"] = "llm_error";
            err_event["request_id"] = request_id;
            err_event["channel"] = "llm.chat";
            err_event["done"] = true;
            Json::Value err_data;
            err_data["error_code"] = 500;
            err_data["error_message"] = "CoreMind HTTP/2 client unavailable";
            err_event["data"] = json_dumps(err_data);
            err_event["timestamp"] = current_timestamp_ms();
            send_frame(client_fd, json_dumps(err_event));
            return;
        }

        auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
        req->SetMethod(CMFrm::COM::POST);
        req->SetReqUrl(target + IM_INFER_CHAT_PATH);
        req->AddHeader("Content-Type", "application/json");
        req->AddBody(body);
        req->SetTimeoutSecond(360);

        // F7.1: 发送前打印完整请求体(原始证据,不截断)
        std::cout << "[uds_proxy] IM_INFER_REQ url=" << target << IM_INFER_CHAT_PATH
                  << " req_body=" << body << std::endl;

        auto httpResult = http2Client->Send(req);
        if (!httpResult || !httpResult->Is2XXStatusCode()) {
            std::string resp_detail = httpResult ? httpResult->GetResponseBody() : "null response";
            std::cerr << "[uds_proxy] IMInferScheduler request failed"
                      << " resp=" << resp_detail << std::endl;
            Json::Value err_event;
            err_event["event_id"] = make_uuid();
            err_event["event_type"] = "llm_error";
            err_event["request_id"] = request_id;
            err_event["channel"] = "llm.chat";
            err_event["done"] = true;
            Json::Value err_data;
            err_data["error_code"] = 502;
            err_data["error_message"] = httpResult ? "IMInferScheduler returned non-2xx" : "IMInferScheduler unreachable";
            err_event["data"] = json_dumps(err_data);
            err_event["timestamp"] = current_timestamp_ms();
            send_frame(client_fd, json_dumps(err_event));
            return;
        }

        std::string respBody = httpResult->GetResponseBody();
        // F7.1: 响应体完整打印(原始证据,不截断)
        std::cout << "[uds_proxy] IM_INFER_RESP resp_body=" << respBody << std::endl;

        // 解析并回传
        Json::Value resp_body;
        Json::CharReaderBuilder reader;
        std::string errors;
        std::istringstream input(respBody);
        if (Json::parseFromStream(reader, input, &resp_body, &errors)) {
            Json::Value final_event;
            final_event["event_id"] = make_uuid();
            final_event["event_type"] = "llm_final";
            final_event["request_id"] = request_id;
            final_event["channel"] = "llm.chat";
            Json::Value final_data;
            final_data["answer"] = resp_body.get("content", resp_body.get("response",
                                resp_body.toStyledString())).asString();
            final_data["finish_reason"] = "stop";
            final_event["data"] = json_dumps(final_data);
            final_event["done"] = true;
            final_event["timestamp"] = current_timestamp_ms();
            send_frame(client_fd, json_dumps(final_event));
        }
        return;
    }
#endif

    // ── 本地开发路径：转发到 OpenAI 兼容 API ──
    httplib::Client cli(llm_api_url_.c_str());
    cli.set_read_timeout(120, 0);

    auto content_type = "application/json";
    // F7.1: 发送前打印完整请求体(原始证据,不截断)
    std::cout << "[uds_proxy] LLM_REQ url=" << llm_api_url_ << "/chat/completions"
              << " req_body=" << body << std::endl;
    auto res = cli.Post("/chat/completions", {{"Content-Type", content_type}}, body, content_type);

    // ... 以下保持原有 OpenAI 响应解析逻辑不变 ...
    if (!res) {
        // F7.1: 失败也打印完整错误证据(可定位)
        std::cerr << "[uds_proxy] LLM_ERR upstream unreachable"
                  << " error=" << httplib::to_string(res.error()) << std::endl;
        Json::Value err_event;
        err_event["event_id"] = make_uuid();
        err_event["event_type"] = "llm_error";
        err_event["request_id"] = request_id;
        err_event["channel"] = "llm.chat";
        err_event["done"] = true;
        Json::Value err_data;
        err_data["error_code"] = 502;
        err_data["error_message"] = "upstream unreachable";
        err_event["data"] = json_dumps(err_data);
        err_event["timestamp"] = current_timestamp_ms();
        send_frame(client_fd, json_dumps(err_event));
        return;
    }

    // 解析响应并回送 UDS 事件
    Json::Value resp_body;
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream input(res->body);
    if (!Json::parseFromStream(reader, input, &resp_body, &errors)) {
        std::cerr << "[uds_proxy] failed to parse upstream response" << std::endl;
        return;
    }

    // 提取第一个 choice 的 content
    auto choices = resp_body["choices"];
    std::string full_content;
    std::string finish_reason = "stop";
    if (choices.isArray() && choices.size() > 0) {
        auto delta = choices[0]["message"]["content"];
        if (delta.isString()) {
            full_content = delta.asString();
        }
        auto fr = choices[0]["finish_reason"];
        if (fr.isString()) {
            finish_reason = fr.asString();
        }
    }

    // 逐 token 发送 llm_token 事件（模拟流式）
    for (char ch : full_content) {
        Json::Value token_event;
        token_event["event_id"] = make_uuid();
        token_event["event_type"] = "llm_token";
        token_event["request_id"] = request_id;
        token_event["channel"] = "llm.chat";
        Json::Value token_data;
        token_data["token"] = std::string(1, ch);
        token_event["data"] = json_dumps(token_data);
        token_event["done"] = false;
        token_event["timestamp"] = current_timestamp_ms();
        send_frame(client_fd, json_dumps(token_event));
    }

    // 发送 llm_final 事件
    Json::Value final_event;
    final_event["event_id"] = make_uuid();
    final_event["event_type"] = "llm_final";
    final_event["request_id"] = request_id;
    final_event["channel"] = "llm.chat";
    Json::Value final_data;
    final_data["answer"] = full_content;
    final_data["finish_reason"] = finish_reason;
    Json::Value usage;
    usage["prompt_tokens"] = resp_body.get("usage", Json::Value())["prompt_tokens"].asInt();
    usage["completion_tokens"] = resp_body.get("usage", Json::Value())["completion_tokens"].asInt();
    usage["total_tokens"] = usage["prompt_tokens"].asInt() + usage["completion_tokens"].asInt();
    final_data["usage"] = usage;
    final_event["data"] = json_dumps(final_data);
    final_event["done"] = true;
    final_event["timestamp"] = current_timestamp_ms();
    send_frame(client_fd, json_dumps(final_event));
}

// ── 帧协议 ──

void UdsModelProxy::send_frame(int client_fd, const std::string& json_data) {
    uint32_t len = static_cast<uint32_t>(json_data.size());
    uint32_t be_len = htonl(len);
    char header[FRAME_HEADER_SIZE];
    std::memcpy(header, &be_len, FRAME_HEADER_SIZE);

    // 使用 MSG_NOSIGNAL 防止 SIGPIPE
    ::send(client_fd, header, FRAME_HEADER_SIZE, MSG_NOSIGNAL);
    ::send(client_fd, json_data.data(), len, MSG_NOSIGNAL);
}

bool UdsModelProxy::recv_frame(int client_fd, std::string& out_json) {
    char header[FRAME_HEADER_SIZE];
    ssize_t n = ::recv(client_fd, header, FRAME_HEADER_SIZE, MSG_WAITALL);
    if (n <= 0) return false;

    uint32_t be_len;
    std::memcpy(&be_len, header, FRAME_HEADER_SIZE);
    uint32_t frame_len = ntohl(be_len);
    if (frame_len > MAX_FRAME_SIZE) {
        std::cerr << "[uds_proxy] frame too large: " << frame_len << std::endl;
        return false;
    }

    std::string buf(frame_len, '\0');
    ssize_t total = 0;
    while (total < static_cast<ssize_t>(frame_len)) {
        ssize_t r = ::recv(client_fd, &buf[total], frame_len - total, 0);
        if (r <= 0) return false;
        total += r;
    }
    out_json = buf;
    return true;
}