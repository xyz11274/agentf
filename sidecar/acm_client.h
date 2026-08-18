#ifndef COREMIND_BRIDGE_ACM_CLIENT_H
#define COREMIND_BRIDGE_ACM_CLIENT_H

// ═══════════════════════════════════════════════════════════════════
// AcmClient — ACM 文件服务客户端（上传/下载）
//
// 设计依据：docs/design/architecture/modules/sidecar/acm-client.md（L4）
// 服务端契约：ACM-master/ACMService/src/sender/file_server_mgr.cpp
//   - 上传：multipart 形式（filename="..." + Content-Length + \r\n\r\n + payload），
//     扩展名白名单 ALLOWED_UPLOAD_FILE_TYPES={jpg,png,log,zip,txt,html,wav,pptx,pdf,docx,xlsx,md}
//   - 下载：HTTP 层恒 200 + body.RetCode 判定；downloadtype: base64 头 → base64 JSON；
//     不带该头 → application/octet-stream 二进制流
//   - 失败信号：RetCode:500 / request file not exist（HTTP 层恒 200，必须按 body 判定）
//
// 双线实现（条件编译，对齐 resolve_handler）：
//   COREMIND_ENABLED=ON  → acm_client.cpp   （SDK HttpClientFactory + Http1Client + AF_SIDECAR_ACM_URL）
//   COREMIND_ENABLED=OFF → acm_client_stub.cpp（httplib 直连 AF_SIDECAR_ACM_URL）
// 公共纯函数（本文件 inline）两线共用，保证契约一致 + 可单元测试。
// ═══════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>
#include <vector>

namespace sidecar {

// ── 结果结构体 ──────────────────────────────────────────────────────

/// 下载结果：成功返回文件 bytes；失败返回 error 详情
struct AcmDownloadResult {
    bool success = false;
    std::vector<uint8_t> content;   // 文件原始 bytes（二进制模式）或 base64 解码后的内容
    std::string error;              // 失败详情（RetCode:500 / request file not exist / 传输错误）
};

/// 上传结果：成功返回 fileUUID；失败返回 error 详情
struct AcmUploadResult {
    bool success = false;
    std::string file_uuid;          // 服务端 Data.fileUUID（非完整 URL）
    std::string error;
};

// ── 路径常量（对齐 ACM-master http_client.h:64-68） ─────────────────

inline constexpr const char* ACM_DOWNLOAD_PATH = "/acm/v1/downloadfile/";
inline constexpr const char* ACM_UPLOAD_PATH = "/acm/v1/uploadfile";

// 上传扩展名白名单（对齐 ACM-master file_server_mgr.h:25-27，2026-08-14 扩展常见产物类型）
inline const std::vector<std::string>& acm_allowed_upload_file_types() {
    static const std::vector<std::string> types = {
        "jpg", "png", "log", "zip", "txt", "html", "wav", "pptx", "pdf", "docx", "xlsx", "md"};
    return types;
}

// ── 公共纯函数（无网络/SDK 依赖，可单元测试，两线共用） ─────────────

/// 构建下载 URL：acm_base + "/acm/v1/downloadfile/" + file_key
inline std::string acm_build_download_url(const std::string& acm_base,
                                          const std::string& file_key) {
    return acm_base + ACM_DOWNLOAD_PATH + file_key;
}

/// 构建上传 URL：acm_base + "/acm/v1/uploadfile"
inline std::string acm_build_upload_url(const std::string& acm_base) {
    return acm_base + ACM_UPLOAD_PATH;
}

/// 失败信号判定（对齐内网示例 + ACM ResponseMsg）：
/// 响应体含 "RetCode":500 或 "request file not exist" 即失败。
/// HTTP 层恒 200，必须按 body 内容判定，不能依赖 HTTP 状态码。
inline bool acm_is_failure_body(const std::string& body) {
    return body.find("\"RetCode\":500") != std::string::npos ||
           body.find("request file not exist") != std::string::npos;
}

/// 从失败响应体提取 error 详情（优先 "request file not exist"，否则 RetCode:500）
inline std::string acm_failure_error(const std::string& body) {
    if (body.find("request file not exist") != std::string::npos) {
        return "request file not exist";
    }
    return "RetCode:500";
}

/// 扩展名白名单校验（对齐 ACM IsAllowedFileType，file_server_mgr.cpp:118-132）
inline bool acm_is_allowed_file_type(const std::string& file_name) {
    const auto dot = file_name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = file_name.substr(dot + 1);
    for (auto& c : ext) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    for (const auto& t : acm_allowed_upload_file_types()) {
        if (ext == t) return true;
    }
    return false;
}

/// 构造 multipart 请求体（对齐服务端 DoHandleUploadFile 解析，file_server_mgr.cpp:230-282）：
///   filename="<file_name>"\r\nContent-Length: <size>\r\n\r\n<payload>
inline std::string acm_build_multipart_body(const std::string& file_name,
                                            const std::vector<uint8_t>& content) {
    std::string body;
    body += "filename=\"";
    body += file_name;
    body += "\"\r\nContent-Length: ";
    body += std::to_string(content.size());
    body += "\r\n\r\n";
    body.append(reinterpret_cast<const char*>(content.data()), content.size());
    return body;
}

/// base64 解码（标准 BASE64，处理 = 填充与空白）。
/// 用于 base64 下载模式下解析 Data.fileContent。解码失败返回空。
inline std::vector<uint8_t> acm_base64_decode(const std::string& in) {
    static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int tbl[256];
    for (int i = 0; i < 256; ++i) tbl[i] = -1;
    for (int i = 0; i < 64; ++i) tbl[static_cast<unsigned char>(B64[i])] = i;

    std::vector<uint8_t> out;
    int val = 0;
    int bits = -8;
    for (unsigned char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        if (tbl[c] < 0) continue;  // 非法字符跳过
        val = (val << 6) | tbl[c];
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

/// 解析二进制下载响应体 → AcmDownloadResult。
/// 二进制模式下响应体即文件 bytes（application/octet-stream）。
/// 失败信号（RetCode:500 / request file not exist）显式识别，不静默当成功。
/// 注意：base64 模式的 JSON 解析与解码由各实现文件内的 parse_base64_download_body
/// 完成（需先提取 Data.fileContent 再解码），本函数仅处理二进制模式与失败信号——
/// 若在此直接对整体响应体做 base64 解码会把 JSON 结构当作内容，属于错误实现。
inline AcmDownloadResult acm_parse_download_body(const std::string& body) {
    AcmDownloadResult r;
    if (acm_is_failure_body(body)) {
        r.success = false;
        r.error = acm_failure_error(body);
        return r;
    }
    r.success = true;
    r.content.assign(body.begin(), body.end());
    return r;
}

// ── AcmClient 类（双线实现） ────────────────────────────────────────

class AcmClient {
public:
    /// 从 ACM 下载文件到内存。
    /// @param file_key 文件标识（拼在 /acm/v1/downloadfile/ 后）
    /// @param base64   true 时请求携带 header `downloadtype: base64`，服务端返回 base64 JSON；
    ///                 false 时请求不带该头，服务端返回 application/octet-stream 二进制流
    static AcmDownloadResult download_file(const std::string& file_key, bool base64 = false);

    /// 上传文件到 ACM。
    /// @param file_name 文件名（写入 multipart 的 filename="..." 字段，服务端校验扩展名白名单）
    /// @param content   文件原始 bytes
    /// @param mime_type 文件 MIME 类型（当前仅记录日志，服务端不校验）
    /// @return 成功时 file_uuid = 服务端 Data.fileUUID
    static AcmUploadResult upload_file(const std::string& file_name,
                                       const std::vector<uint8_t>& content,
                                       const std::string& mime_type);

    /// 从 url 下载产物内容到内存（L4 §4.2 产物内容获取，url 优先路径）。
    /// 返回原始 bytes（std::string 承载）；失败返回空串（调用方降级 base64 兜底）。
    /// 双线实现：stub = httplib GET；生产 = SDK Http1Client GET。
    static std::string fetch_url(const std::string& url);
};

}  // namespace sidecar

#endif  // COREMIND_BRIDGE_ACM_CLIENT_H
