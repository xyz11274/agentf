#include "acm_client.h"

// ═══════════════════════════════════════════════════════════════════
// acm_client_stub.cpp — dev 实现（COREMIND_ENABLED=OFF 时编译）
//
// 使用 httplib 直接调用 ACM 服务端（本地无 CoreMind SDK）。
// ACM 地址来自 AF_SIDECAR_ACM_URL 环境变量，默认 http://127.0.0.1:23768
// （指向 upstream_mock 的 ACM 端点，本地零新增服务）。
//
// 契约对齐：见 acm_client.h 头部注释（ACM-master file_server_mgr.cpp）。
// 日志对齐 L4 §5.2：事件前缀 ACM_DL_REQ/RESP/ERR、ACM_UP_REQ/RESP/ERR，
// 含 elapsed_ms 与完整响应体（F7.1），base64 图片字段级截断（F7.2）。
// ═══════════════════════════════════════════════════════════════════

#include <cstdlib>
#include <chrono>
#include <iostream>
#include <json/json.h>
#include <httplib.h>
#include "router_logic.h"  // log_timestamp / truncate_base64_in_text

namespace sidecar {

namespace {

/// 获取 ACM 服务地址：AF_SIDECAR_ACM_URL，默认 upstream_mock。
std::string acm_base_url() {
    const char* env = std::getenv("AF_SIDECAR_ACM_URL");
    if (env != nullptr && *env != '\0') {
        return env;
    }
    return "http://127.0.0.1:23768";
}

/// 解析 base64 下载模式的 JSON 响应，提取 Data.fileContent 并解码。
/// 返回 true 表示成功；false 时 out_error 携带失败详情（含完整响应体供定位）。
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

}  // namespace

AcmDownloadResult AcmClient::download_file(const std::string& file_key, bool base64) {
    AcmDownloadResult result;
    const std::string base = acm_base_url();
    const std::string url = acm_build_download_url(base, file_key);
    const auto t0 = std::chrono::steady_clock::now();

    // 请求前日志（F7.1：发送之前打印，连接失败也能看到请求参数）
    std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_DL_REQ"
              << " url=" << url << " file_key=" << file_key
              << " base64=" << (base64 ? "true" : "false") << std::endl;

    httplib::Client cli(base);
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(30, 0);

    httplib::Headers headers;
    if (base64) {
        headers.emplace("downloadtype", "base64");
    }

    auto resp = cli.Get(url.c_str(), headers);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();

    if (!resp) {
        result.success = false;
        result.error = "httplib error: " + httplib::to_string(resp.error());
        // F7.1 失败也打印完整错误证据
        std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_DL_ERR"
                  << " url=" << url << " error=" << result.error
                  << " elapsed_ms=" << elapsed_ms << std::endl;
        return result;
    }

    const std::string& body = resp->body;

    // HTTP 状态码防御：ACM 契约 HTTP 恒 200，但真实环境若返回非 200（如 404/500 错误页，
    // 非 RetCode JSON），body 判定会误判成功——非 200 一律按失败处理（M3 失败路径完整）。
    if (resp->status != 200) {
        result.success = false;
        result.error = "ACM http status " + std::to_string(resp->status) + ": " + body;
        std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_DL_ERR"
                  << " url=" << url << " error=" << result.error
                  << " elapsed_ms=" << elapsed_ms << std::endl;
        return result;
    }

    // 失败信号识别（HTTP 恒 200，必须按 body 判定）
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
    return result;
}

AcmUploadResult AcmClient::upload_file(const std::string& file_name,
                                       const std::vector<uint8_t>& content,
                                       const std::string& mime_type) {
    AcmUploadResult result;
    const std::string base = acm_base_url();
    const std::string url = acm_build_upload_url(base);
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

    httplib::Client cli(base);
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(30, 0);

    const std::string multipart = acm_build_multipart_body(file_name, content);
    auto resp = cli.Post(url.c_str(), multipart, "application/octet-stream");
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();

    if (!resp) {
        result.success = false;
        result.error = "httplib error: " + httplib::to_string(resp.error());
        std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_UP_ERR"
                  << " url=" << url << " error=" << result.error
                  << " elapsed_ms=" << elapsed_ms << std::endl;
        return result;
    }

    const std::string& body = resp->body;

    // HTTP 状态码防御（与下载一致，M3 失败路径完整）：非 200 一律按失败处理
    if (resp->status != 200) {
        result.success = false;
        result.error = "ACM http status " + std::to_string(resp->status) + ": " + body;
        std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_UP_ERR"
                  << " url=" << url << " error=" << result.error
                  << " elapsed_ms=" << elapsed_ms << std::endl;
        return result;
    }

    // 失败信号识别
    if (acm_is_failure_body(body)) {
        result.success = false;
        result.error = acm_failure_error(body);
        std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_UP_ERR"
                  << " url=" << url << " error=" << result.error
                  << " elapsed_ms=" << elapsed_ms
                  << " resp_body=" << body << std::endl;
        return result;
    }

    // 成功响应解析 Data.fileUUID；解析异常按失败处理（不猜测字段）
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
    return result;
}

std::string AcmClient::fetch_url(const std::string& url) {
    if (url.empty()) return "";
    const auto t0 = std::chrono::steady_clock::now();
    std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_ART_DL"
              << " url=" << url << " via=url" << std::endl;

    httplib::Client cli(url.substr(0, url.find('/') == std::string::npos ? url.size()
                                 : url.find('/', url.find("://") == std::string::npos ? 0 : url.find("://") + 3)));
    cli.set_connection_timeout(10, 0);
    cli.set_read_timeout(30, 0);
    auto resp = cli.Get(url.c_str());
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
    if (!resp || resp->status != 200) {
        const std::string err = !resp ? ("httplib error: " + httplib::to_string(resp.error()))
                                      : ("http status " + std::to_string(resp->status));
        std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_ART_DL_ERR"
                  << " url=" << url << " error=" << err
                  << " elapsed_ms=" << elapsed_ms << std::endl;
        return "";
    }
    std::cerr << "[" << log_timestamp() << "] [acm_client] ACM_ART_DL_OK"
              << " url=" << url << " size=" << resp->body.size()
              << " elapsed_ms=" << elapsed_ms << std::endl;
    return resp->body;
}

}  // namespace sidecar
