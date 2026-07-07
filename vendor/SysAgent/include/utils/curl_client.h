//
// Created by c00888021 on 2026/1/22.
//

#ifndef SYSAGENT_CURL_CLIENT_H
#define SYSAGENT_CURL_CLIENT_H

#include <iostream>
#include <string>
#include <curl/curl.h>
#include <cstring>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>

namespace SYSAGENT {

class CurlClient {
public:
    CurlClient()
    {
        call_once(global_init_flag, GlobalInit);
    }

    ~CurlClient()
    {
        if (headers) curl_slist_free_all(headers);
        if (curl_handle) curl_easy_cleanup(curl_handle);
    }

    void AddHeader(const std::string& header);
    std::string Request(const std::string& url, const std::string& post_data = "");
    std::string Get(const std::string& url) { return Request(url); }
    std::string Post(const std::string& url, const std::string& data) { return Request(url, data); }
    void SetSocks5Proxy(const std::string& proxyHost, int proxyPort);

private:
    static thread_local CURL* curl_handle;
    static thread_local curl_slist* headers;
    static thread_local std::string response_buf;
    static thread_local char err_buf[CURL_ERROR_SIZE];

    static std::once_flag global_init_flag;
    static std::atomic<bool> is_inited;

    // 响应数据回调函数
    static size_t WriteCallback(void* ptr, size_t size, size_t nmemb, void* self);
    // 初始化当前线程的curl句柄（只执行1次）
    void InitHandle();
    // curl全局初始化（线程安全，仅执行1次）
    static void GlobalInit();
};

}

#endif //SYSAGENT_CURL_CLIENT_H
