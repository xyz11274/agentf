//
// Created by c00888021 on 2025/11/26.
//
#include <ctime>
#include <filesystem>
#include <iostream>
#include <fstream>

#include "securec.h"
#include "curl/curl.h"
#include "framework_common/utils/base64_util.h"
#include "framework_common/utils/defer_util.h"
#include "framework_common/timermanager/timer_manager.h"

#include "utils/common_func.h"
#include "utils/logger.h"
#include "aisf_platform/common_macro.h"
#include "queue/QueueManager.h"
#include "config/Config.h"

namespace SYSAGENT {

// 读取图片并返回 Base64 编码的字符串
std::string CommonFunc::Pic2Base64(const std::string &pic_path)
{
    char *canonicalPath = realpath(pic_path.c_str(), nullptr);
    if (canonicalPath == nullptr) {
        LOG_INFO_EX("File path is invalid(%s).", pic_path.c_str());
        return "";
    }
    CMFrm::Utils::Defer defer([canonicalPath]() {
        if (canonicalPath != nullptr) {
            free(canonicalPath);
        }
    });
    // 打开图片文件
    std::ifstream img_file(canonicalPath, std::ios::binary);
    if (!img_file.is_open()) {
        LOG_INFO_EX("Failed to open file: %s", canonicalPath);
        return "";
    }

    // 读取文件内容到缓冲区
    std::vector<unsigned char> buffer((std::istreambuf_iterator<char>(img_file)),
                                      std::istreambuf_iterator<char>());

    // 编码为 Base64
    std::string strBuffer(buffer.begin(), buffer.end());
    return CMFrm::Utils::Base64Encode(strBuffer);
}

bool CommonFunc::EnsureDirectoryExists(const std::string& path)
{
    std::filesystem::path dirPath(path);
    dirPath = dirPath.parent_path();
    if (!std::filesystem::exists(dirPath)) {
        if (std::filesystem::create_directories(dirPath)) {
            LOG_INFO_EX("Directory created: %s", dirPath.c_str());
            return true;
        } else {
            LOG_INFO_EX("Failed to create directory: %s", dirPath.c_str());
            return false;
        }
    }
    return true;
}

// 文件下载
std::string CommonFunc::FileDownload(const std::string &imageUrl, const std::string &acmIP,
                                     const std::string &fileDirection)
{
    if (acmIP.empty()) {
        LOG_INFO_EX("Get acm instance failed.");
        return "";
    }
    std::string imageFinalUrl = "http://" + acmIP + ":8080/acm/v1/downloadfile/" + imageUrl;
    std::string imagePath = fileDirection + "/" + imageUrl;
    EnsureDirectoryExists(imagePath);
    LOG_INFO_EX("start download file. imageFinalUrl is %s, imagePath is %s", imageFinalUrl.c_str(), imagePath.c_str());
    // 回调函数，用于将下载的数据写入文件
    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_INFO_EX("Failed to initialize CURL.");
        return "";
    }

    char *imageRealPath = realpath(imagePath.c_str(), nullptr);
    if (imageRealPath == nullptr) {
        LOG_INFO_EX("imagepath is invalid:%s", imagePath.c_str());
        return "";
    }
    FILE* fp = fopen(imagePath.c_str(), "wb");
    if (!fp) {
        LOG_INFO_EX("Failed to open output file: %s", imagePath.c_str());
        curl_easy_cleanup(curl);
        return "";
    }

    // 设置 URL
    CURLcode res = curl_easy_setopt(curl, CURLOPT_URL, imageFinalUrl.c_str());
    if (res != CURLE_OK) {
        LOG_INFO_EX("Failed to set URL: %s", curl_easy_strerror(res));
        fclose(fp);
        curl_easy_cleanup(curl);
        return "";
    }

    // 设置写入数据的文件指针
    res = curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL); // 使用默认的 fwrite

    // 设置请求超时时间为2s
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);

    // 执行下载
    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        if (res == CURLE_OPERATION_TIMEDOUT) {
            LOG_INFO_EX("curl_easy_perform time out: %s.", curl_easy_strerror(res));
        }
        LOG_INFO_EX("Failed to perform request: %s", curl_easy_strerror(res));
        fclose(fp);
        curl_easy_cleanup(curl);
        return "";
    }

    fclose(fp);
    curl_easy_cleanup(curl);

    LOG_INFO_EX("Image downloaded successfully: %s", imagePath.c_str());
    return imagePath; // 返回下载文件的路径
}

size_t CommonFunc::GetStringHash(const std::string& userName, uint32_t mod)
{
    if (mod == 0) {
        LOG_ERR_EX("mod zero invalid: %d", mod);
        return 0;
    }
    std::hash<std::string> userNameHash;
    size_t hashValue = userNameHash(userName);
    return hashValue % mod;
}

void CommonFunc::WaitForPromise(std::shared_ptr<std::promise<std::string>> promise, std::string& query,
                                std::string description, std::shared_ptr<AccessCtrl> accessCtrl)
{
    CHECK_NULL_LOG_ERROR_RETURN_VOID(promise, "promise is null");
    try {
        std::future<std::string> future = promise->get_future();
        std::future_status status = future.wait_for(std::chrono::seconds(15));
        if (status == std::future_status::ready) {
            QueueManager::getInstance().EventMark("info", description, "success", "success", accessCtrl);
            query = future.get();
        } else {
            QueueManager::getInstance().EventMark("info", description, "failed", "failed", accessCtrl);
            LOG_ERR_EX("wait for promise failed:%s", query.c_str());
        }
    } catch (const std::exception& e) {
        LOG_ERR_EX("error is :%s", e.what());
    }
}

std::string CommonFunc::GetCurrentDateWithWeekday()
{
    auto now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    std::tm* now_tm = std::localtime(&now_time_t);
    static const std::string weekdays_zh[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    std::ostringstream oss;
    oss << std::put_time(now_tm, "%Y-%m-%d ") << weekdays_zh[now_tm->tm_wday];
    return oss.str();
}

bool IsSpace(char ch)
{
    return std::isspace(static_cast<unsigned char>(ch));
}

std::string CommonFunc::Trim(const std::string& str)
{
    auto start = std::find_if_not(str.begin(), str.end(), IsSpace);
    auto end = std::find_if_not(str.rbegin(), str.rend(), IsSpace).base();
    if (start >= end) {
        return {};
    }

    return std::string(start, end);
}

std::string CommonFunc::ExtractJsonString(const std::string &response)
{
    try {
        std::regex regex("```json([\\s\\S]*)```");
        std::smatch match;
        if (std::regex_search(response, match, regex) && match.size() > 1) {
            // 提取匹配的内容（即标签内的部分）
            std::string result = match[1];
            CMFrm_Error("==============:%s", result.c_str());
            return Trim(result);
        }
        return response;
    } catch (...) {
        CMFrm_Error("Regex search error. The string(%s) is invalid.", response.c_str());
        return response;
    }
}

bool CommonFunc::AppendDebugInfoToFile(const std::string& userId, const std::string& timeStamp,
                                       const std::string& debugInfo)
{
    std::string debugInfoFilePath = ConfigManager::getInstance().
            getEnvInfo("MODEL_LOG_PATH", "/opt/coremind/logs/SysAgent/ModelLog");
    debugInfoFilePath += "/" + userId + "/" + timeStamp;
    LOG_INFO_EX("debugInfoFilePath is:%s", debugInfoFilePath.c_str());
    // 创建目录（如果不存在）
    std::filesystem::path filePath(debugInfoFilePath);
    std::filesystem::path parentDirPath = filePath.parent_path();
    if (!std::filesystem::exists(parentDirPath)) {
        try {
            if (!std::filesystem::create_directories(parentDirPath)) {
                throw std::runtime_error("create_directories failed for file: " + debugInfoFilePath);
            }
        } catch (const std::filesystem::filesystem_error& e) {
            throw std::runtime_error("create_directories catch filesystem_error for file: " + debugInfoFilePath
                                     + ", errMsg: " + e.what());
        }
    }
    std::ofstream file;
    try {
        // 使用app模式打开文件并追加内容
        file.open(filePath, std::ios::app);
        if (!file.is_open()) {
            LOG_ERR("Can not open file %s", debugInfoFilePath.c_str());
            return false;
        }

        file << "[" << timeStamp << "][" << userId <<"]\n" << debugInfo << "\n";
        file.close();
    } catch (const std::exception& e) {
        if (file.is_open()) {
            file.close();
        }
        throw std::runtime_error("catch exception when open file: " + debugInfoFilePath + ", errMsg: " + e.what());
    }
    return true;
}

}