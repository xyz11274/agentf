#include "acm_client.h"

// ═══════════════════════════════════════════════════════════════════
// acm_client.cpp — 生产实现（COREMIND_ENABLED=ON 时编译）
//
// 使用 CoreMind C++ SDK 的 HttpClient（内网预编译依赖，本地无头文件）：
//   - HttpClient::GetAcmUrl(path)         — 路径 → 补全 host/port 的完整 URL（内部服务发现）
//   - HttpClient::Http1CertSendRequest(...)— HTTP/1.1 + 证书（mTLS）请求
// 签名依据（docs/research/acm集成.md 内网示例 + ACM-master http_client.cpp:338-341
// 同类方法 GetAISFMgrUrl）推断；真实编译在华为 CI/CD 环境（PLATFORM_DIR）。
//
// 本地验证策略：本文件不在本地编译（无 SDK），契约正确性由 acm_client_stub.cpp
// 共用同一套公共纯函数（acm_client.h inline）保证；真实编译由 CI/CD 完成。
// ═══════════════════════════════════════════════════════════════════

#include <chrono>
#include <iostream>
#include <map>
#include <sstream>

#ifdef COREMIND_ENABLED
#include <json/json.h>
#include "router_logic.h"  // log_timestamp / truncate_base64_in_text
#endif

namespace sidecar {

namespace {

#ifdef COREMIND_ENABLED

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

/// SDK HttpClient 声明（依据内网示例推断签名，见文件头注释）。
/// 真实定义在 CoreMind SDK 头文件（PLATFORM_DIR/include），编译期校验。
namespace sdk_http {
std::string GetAcmUrl(const std::string& path);
bool Http1CertSendRequest(const std::string& url,
                          const std::string& body,
                          const std::map<std::string, std::string>& headers,
                          const std::function<void(const std::string&)>& callback,
                          int method /* CMFrm::COM::GET / POST */);
}  // namespace sdk_http

#endif  // COREMIND_ENABLED

}  // namespace

AcmDownloadResult AcmClient::download_file(const std::string& file_key, bool base64) {
    AcmDownloadResult result;
#ifdef COREMIND_ENABLED
    const std::string url = sdk_http::GetAcmUrl(acm_build_download_url("", file_key));
    const auto t0 = std::chrono::steady_clock::now();

    std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_DL_REQ"
              << " url=" << url << " file_key=" << file_key
              << " base64=" << (base64 ? "true" : "false") << std::endl;

    std::map<std::string, std::string> headers;
    if (base64) {
        headers.emplace("downloadtype", "base64");
    }

    std::string body;
    // method=GET：CMFrm::COM::GET 枚举值（0）。SDK 头文件到位后替换为枚举。
    const bool ok = sdk_http::Http1CertSendRequest(
        url, "", headers, [&](const std::string& chunk) { body += chunk; }, 0);

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();

    if (!ok) {
        result.success = false;
        result.error = "Http1CertSendRequest failed";
        std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_DL_ERR"
                  << " url=" << url << " error=" << result.error
                  << " elapsed_ms=" << elapsed_ms << std::endl;
        return result;
    }

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
        result.content.assign(body.begin(), body.end());
    }

    result.success = true;
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
    const std::string url = sdk_http::GetAcmUrl(acm_build_upload_url(""));
    const auto t0 = std::chrono::steady_clock::now();

    if (!acm_is_allowed_file_type(file_name)) {
        result.success = false;
        result.error = "file type not allowed: " + file_name;
        std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_UP_ERR"
                  << " url=" << url << " file_name=" << file_name
                  << " error=" << result.error << std::endl;
        return result;
    }

    std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_UP_REQ"
              << " url=" << url << " file_name=" << file_name
              << " content_len=" << content.size()
              << " mime_type=" << mime_type << std::endl;

    std::map<std::string, std::string> headers;
    const std::string multipart = acm_build_multipart_body(file_name, content);

    std::string body;
    // method=POST：CMFrm::COM::POST 枚举值（1）。SDK 头文件到位后替换为枚举。
    const bool ok = sdk_http::Http1CertSendRequest(
        url, multipart, headers, [&](const std::string& chunk) { body += chunk; }, 1);

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();

    if (!ok) {
        result.success = false;
        result.error = "Http1CertSendRequest failed";
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
    // method=GET：CMFrm::COM::GET 枚举值（0）。SDK 头文件到位后替换为枚举。
    const bool ok = sdk_http::Http1CertSendRequest(
        url, "", headers, [&](const std::string& chunk) { content += chunk; }, 0);

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
    if (!ok) {
        std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_ART_DL_ERR"
                  << " url=" << url << " error=Http1CertSendRequest failed"
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
