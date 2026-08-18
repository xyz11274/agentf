#include "acm_client.h"

// ═══════════════════════════════════════════════════════════════════
// acm_client.cpp — 生产实现（COREMIND_ENABLED=ON 时编译）

// ═══════════════════════════════════════════════════════════════════

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>

#ifdef COREMIND_ENABLED
#include <json/json.h>
#include <framework_com/http_client_factory.h>  // HttpClientFactory / Http1Client / Http1ClientRequest / Http1ClientConfig
#include "router_logic.h"  // log_timestamp / truncate_base64_in_text
#endif

namespace sidecar {

namespace {

#ifdef COREMIND_ENABLED

/// 获取 ACM 服务地址：AF_SIDECAR_ACM_URL，默认 upstream_mock（与 stub 契约一致）。
/// 生产部署通过环境变量注入真实 ACM 地址（设计文档回退方案，SDK 无 GetAcmUrl 服务发现）。
std::string acm_base_url() {
    const char* env = std::getenv("AF_SIDECAR_ACM_URL");
    if (env != nullptr && *env != '\0') {
        return env;
    }
    return "http://127.0.0.1:23768";
}

/// 获取（懒注册）Http1Client。name="acm_default"，配置对齐 SysAgent http_client.cpp 的
/// RegisterHttp1ClientConfig 模式（keepAlive + 超时 + 连接数）。首次调用注册，之后复用。
std::shared_ptr<CMFrm::COM::Http1Client> get_http1_client() {
    auto factory = CMFrm::COM::HttpClientFactory::GetHttpClientFactory();
    auto client = factory->GetHttp1Client("acm_default");
    if (client) {
        return client;
    }
    auto config = std::make_shared<CMFrm::COM::Http1ClientConfig>();
    config->name = "acm_default";
    config->keepAlive = true;
    config->keepAliveTimeoutSecond = 30;
    config->maxConnectNum = 2;
    config->timeoutSecond = 30;
    factory->RegisterHttp1ClientConfig(config);
    return factory->GetHttp1Client("acm_default");
}

/// 发送 HTTP/1.1 请求，返回响应体字符串。
/// 失败（客户端不可用 / 非 2XX）时返回空串并通过 out_error 携带可定位详情（M3）。
/// 注意：ACM 契约 HTTP 恒 200、真实状态在 body.RetCode——此处 Is2XXStatusCode 仅作
/// 传输层防御（非 200 页面），业务失败信号仍由调用方按 body 判定（与 stub 一致）。
std::string http1_send(const std::string& url,
                       const std::string& body,
                       const std::map<std::string, std::string>& headers,
                       CMFrm::COM::HttpMethod method,
                       std::string* out_error) {
    auto client = get_http1_client();
    if (!client) {
        if (out_error) *out_error = "GetHttp1Client failed (acm_default)";
        return "";
    }
    auto req = std::make_shared<CMFrm::COM::Http1ClientRequest>();
    req->SetMethod(method);
    req->SetReqUrl(url);
    for (const auto& kv : headers) {
        req->AddHeader(kv.first, kv.second);
    }
    req->SetBody(body);
    auto resp = client->Send(req);
    if (!resp) {
        if (out_error) *out_error = "Http1Client::Send returned null";
        return "";
    }
    if (!resp->Is2XXStatusCode()) {
        if (out_error) *out_error = "ACM http status " + std::to_string(resp->GetStatusCode());
        return "";
    }
    return resp->GetResponseBody();
}

/// 解析 base64 下载模式的 JSON 响应，提取 Data.fileContent 并解码。
bool parse_base64_download_body(const std::string& body, AcmDownloadResult& out,
                                std::string* out_error) {
    Json::Value root;
    Json::CharReaderBuilder reader;
    std::string errs;
    std::istringstream stream(body);
    if (!Json::parseFromStream(reader, stream, &root, &errs)) {
        out_error->assign("base64 download JSON parse error: " + errs + " | body=" + body);
        return false;
    }
    const auto& data = root["Data"];
    if (!data.isObject() || !data["fileContent"].isString()) {
        out_error->assign("base64 download missing Data.fileContent | body=" + body);
        return false;
    }
    out.content = acm_base64_decode(data["fileContent"].asString());
    if (out.content.empty() && data["fileContent"].asString().empty()) {
        out_error->assign("base64 download decoded empty content");
        return false;
    }
    return true;
}

/// 解析上传响应 JSON，提取 Data.fileUUID。
bool parse_upload_response(const std::string& body, AcmUploadResult& out,
                           std::string* out_error) {
    Json::Value root;
    Json::CharReaderBuilder reader;
    std::string errs;
    std::istringstream stream(body);
    if (!Json::parseFromStream(reader, stream, &root, &errs)) {
        out_error->assign("upload response JSON parse error: " + errs + " | body=" + body);
        return false;
    }
    const auto& data = root["Data"];
    if (!data.isObject() || !data["fileUUID"].isString()) {
        out_error->assign("upload response missing Data.fileUUID | body=" + body);
        return false;
    }
    out.file_uuid = data["fileUUID"].asString();
    return true;
}

#endif  // COREMIND_ENABLED

}  // namespace

AcmDownloadResult AcmClient::download_file(const std::string& file_key, bool base64) {
    AcmDownloadResult result;
#ifdef COREMIND_ENABLED
    const std::string url = acm_build_download_url(acm_base_url(), file_key);
    const auto t0 = std::chrono::steady_clock::now();

    // 请求前日志（F7.1：发送之前打印，连接失败也能看到请求参数）
    std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_DL_REQ"
              << " url=" << url << " file_key=" << file_key
              << " base64=" << (base64 ? "true" : "false") << std::endl;

    std::map<std::string, std::string> headers;
    if (base64) {
        headers.emplace("downloadtype", "base64");
    }

    std::string err;
    const std::string body = http1_send(url, "", headers, CMFrm::COM::HttpMethod::GET, &err);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();

    if (!err.empty()) {
        result.success = false;
        result.error = err;
        // F7.1 失败也打印完整错误证据
        std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_DL_ERR"
                  << " url=" << url << " error=" << result.error
                  << " elapsed_ms=" << elapsed_ms << std::endl;
        return result;
    }

    // 失败信号识别（HTTP 恒 200，必须按 body 判定；对齐 stub 与 ACM 服务端契约）
    if (acm_is_failure_body(body)) {
        result.success = false;
        result.error = acm_failure_error(body);
        std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_DL_ERR"
                  << " url=" << url << " error=" << result.error
                  << " elapsed_ms=" << elapsed_ms
                  << " resp_body=" << body << std::endl;
        return result;
    }

    if (base64) {
        std::string parse_err;
        if (!parse_base64_download_body(body, result, &parse_err)) {
            result.success = false;
            result.error = parse_err;
            std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_DL_ERR"
                      << " url=" << url << " error=" << result.error
                      << " elapsed_ms=" << elapsed_ms << std::endl;
            return result;
        }
    } else {
        // 二进制模式：响应体即文件 bytes（原始字节，禁止字符串转换）
        result.content.assign(body.begin(), body.end());
    }

    result.success = true;
    // F7.2：响应体含 base64 时截断展示，完整内容已在 dumper 落盘（F7.1）
    std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_DL_RESP"
              << " url=" << url << " success=true"
              << " content_len=" << result.content.size()
              << " elapsed_ms=" << elapsed_ms
              << " resp_body=" << truncate_base64_in_text(body) << std::endl;
#else
    (void)file_key;
    (void)base64;
    result.error = "acm_client.cpp compiled in stub mode (COREMIND_ENABLED=OFF)";
#endif
    return result;
}

AcmUploadResult AcmClient::upload_file(const std::string& file_name,
                                       const std::vector<uint8_t>& content,
                                       const std::string& mime_type) {
    AcmUploadResult result;
#ifdef COREMIND_ENABLED
    const std::string url = acm_build_upload_url(acm_base_url());
    const auto t0 = std::chrono::steady_clock::now();

    // 上传前扩展名白名单校验（对齐 ACM IsAllowedFileType，避免 400）
    if (!acm_is_allowed_file_type(file_name)) {
        result.success = false;
        result.error = "file type not allowed: " + file_name;
        std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_UP_ERR"
                  << " url=" << url << " file_name=" << file_name
                  << " error=" << result.error << std::endl;
        return result;
    }

    // 请求前日志（F7.1）
    std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_UP_REQ"
              << " url=" << url << " file_name=" << file_name
              << " content_len=" << content.size()
              << " mime_type=" << mime_type << std::endl;

    std::map<std::string, std::string> headers;
    const std::string multipart = acm_build_multipart_body(file_name, content);

    std::string err;
    const std::string body = http1_send(url, multipart, headers, CMFrm::COM::HttpMethod::POST, &err);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();

    if (!err.empty()) {
        result.success = false;
        result.error = err;
        std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_UP_ERR"
                  << " url=" << url << " error=" << result.error
                  << " elapsed_ms=" << elapsed_ms << std::endl;
        return result;
    }

    if (acm_is_failure_body(body)) {
        result.success = false;
        result.error = acm_failure_error(body);
        std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_UP_ERR"
                  << " url=" << url << " error=" << result.error
                  << " elapsed_ms=" << elapsed_ms
                  << " resp_body=" << body << std::endl;
        return result;
    }

    std::string parse_err;
    if (!parse_upload_response(body, result, &parse_err)) {
        result.success = false;
        result.error = parse_err;
        std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_UP_ERR"
                  << " url=" << url << " error=" << result.error
                  << " elapsed_ms=" << elapsed_ms << std::endl;
        return result;
    }

    result.success = true;
    std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_UP_RESP"
              << " url=" << url << " success=true"
              << " file_uuid=" << result.file_uuid
              << " elapsed_ms=" << elapsed_ms
              << " resp_body=" << body << std::endl;
#else
    (void)file_name;
    (void)content;
    (void)mime_type;
    result.error = "acm_client.cpp compiled in stub mode (COREMIND_ENABLED=OFF)";
#endif
    return result;
}

std::string AcmClient::fetch_url(const std::string& url) {
    std::string content;
#ifdef COREMIND_ENABLED
    if (url.empty()) return "";
    const auto t0 = std::chrono::steady_clock::now();
    std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_ART_DL"
              << " url=" << url << " via=url" << std::endl;

    std::map<std::string, std::string> headers;
    std::string err;
    content = http1_send(url, "", headers, CMFrm::COM::HttpMethod::GET, &err);

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
    if (!err.empty()) {
        std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_ART_DL_ERR"
                  << " url=" << url << " error=" << err
                  << " elapsed_ms=" << elapsed_ms << std::endl;
        return "";
    }
    std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_ART_DL_OK"
              << " url=" << url << " size=" << content.size()
              << " elapsed_ms=" << elapsed_ms << std::endl;
#else
    (void)url;
    std::cerr << "[acm_client] fetch_url unavailable in stub mode (use acm_client_stub.cpp)" << std::endl;
#endif
    return content;
}

}  // namespace sidecar
