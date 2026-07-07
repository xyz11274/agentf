//
// Created by c00888021 on 2026/1/22.
//

#include "utils/curl_client.h"
#include "utils/logger.h"
#include "framework_common/constants/number_constant.h"
#include "framework_com/http_status.h"

using namespace std;

namespace SYSAGENT {

    thread_local CURL* CurlClient::curl_handle = nullptr;
    thread_local curl_slist* CurlClient::headers = nullptr;
    thread_local string CurlClient::response_buf;
    thread_local char CurlClient::err_buf[CURL_ERROR_SIZE] = {0};
    once_flag CurlClient::global_init_flag;
    atomic<bool> CurlClient::is_inited(false);

    size_t CurlClient::WriteCallback(void* ptr, size_t size, size_t nmemb, void* self)
    {
        CurlClient* client = static_cast<CurlClient*>(self);
        size_t len = size * nmemb;
        client->response_buf.append((char*)ptr, len);
        return len;
    }

    void CurlClient::InitHandle()
    {
        if (curl_handle) return;
        curl_handle = curl_easy_init();
        headers = nullptr;
        memset_s(err_buf, sizeof(err_buf), 0, sizeof(err_buf));

        if (curl_handle) {
            // ========== 核心配置：忽略SSL证书校验 【必配】 ==========
            curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 0L);

            // ========== 连接复用+性能优化 ==========
            curl_easy_setopt(curl_handle, CURLOPT_FORBID_REUSE, 0L);    // 允许连接复用（长连接）
            curl_easy_setopt(curl_handle, CURLOPT_TCP_KEEPALIVE, 1L);   // TCP保活，维持长连接
            curl_easy_setopt(curl_handle, CURLOPT_TCP_KEEPIDLE, 5);     // 5秒无数据发保活包
            curl_easy_setopt(curl_handle, CURLOPT_TCP_KEEPINTVL, 2);    // 保活包间隔2秒

            // ========== 通用配置 ==========
            curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, this);
            curl_easy_setopt(curl_handle, CURLOPT_ERRORBUFFER, err_buf);
            curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, CMFrm::Constants::Const10);
            curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, CMFrm::Constants::Const60);
            curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl_handle, CURLOPT_NOSIGNAL, 1L);        // 线程安全必配
            curl_easy_setopt(curl_handle, CURLOPT_ACCEPT_ENCODING, "gzip, deflate"); // 支持压缩
        }
    }

    void CurlClient::GlobalInit()
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        is_inited.store(true);
    }

    void CurlClient::AddHeader(const std::string& header)
    {
        InitHandle();
        headers = curl_slist_append(headers, header.c_str());
        curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);
    }

    string CurlClient::Request(const string& url, const string& post_data)
    {
        call_once(global_init_flag, GlobalInit);
        InitHandle();
        if (!curl_handle) return "[ERROR] CURL句柄初始化失败";

        // 清空缓冲区
        response_buf.clear();
        memset_s(err_buf, sizeof(err_buf), 0, sizeof(err_buf));

        // 设置请求地址
        curl_easy_setopt(curl_handle, CURLOPT_URL, url.c_str());

        // 设置请求方式
        if (post_data.empty()) {
            curl_easy_setopt(curl_handle, CURLOPT_HTTPGET, 1L);
        } else {
            curl_easy_setopt(curl_handle, CURLOPT_POST, 1L);
            curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, post_data.c_str());
            curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, post_data.size());
        }

        // 执行请求
        CURLcode res = curl_easy_perform(curl_handle);
        if (res != CURLE_OK) {
            LOG_INFO_EX("curl failed, errMsg is: %s", err_buf);
            return "";
        }

        // 获取HTTP状态码
        long code = 0;
        curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &code);
        LOG_INFO_EX("curl httpcode is: %ld", code);
        if (code != CMFrm::COM::HttpStatus::HTTP200) {
            LOG_INFO_EX("curl result is:%s", response_buf.c_str());
            return "";
        }
        return response_buf;
    }

    void CurlClient::SetSocks5Proxy(const std::string& proxyHost, int proxyPort)
    {
        std::string proxy = proxyHost + ":" + std::to_string(proxyPort);
        curl_easy_setopt(curl_handle, CURLOPT_PROXY, proxy.c_str());
        curl_easy_setopt(curl_handle, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME);
    }
}