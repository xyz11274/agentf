//
// Created by c00888021 on 2025/11/26.
//

#ifndef SYSAGENT_COMMON_FUNC_H
#define SYSAGENT_COMMON_FUNC_H

#include <string>
#include <future>
#include "aisf_platform/parser/aisf_msg_ctrl.h"

namespace SYSAGENT {

class CommonFunc final {
public:
    static std::string FileDownload(const std::string &imageUrl, const std::string &acmIP,
                                    const std::string &fileDirection);
    static std::string Pic2Base64(const std::string &pic_path);
    static bool EnsureDirectoryExists(const std::string& path);
    static void WaitForPromise(std::shared_ptr<std::promise<std::string>> promise, std::string& query,
                               std::string description, std::shared_ptr<AccessCtrl> accessCtrl);
    static size_t GetStringHash(const std::string& userName, uint32_t mod);
    static std::string GetCurrentDateWithWeekday();
    static std::string Trim(const std::string& str);
    static std::string ExtractJsonString(const std::string &response);
    static bool AppendDebugInfoToFile(const std::string& userId, const std::string& timeStamp,
                                      const std::string& debugInfo);
};

}

#endif // SYSAGENT_COMMON_FUNC_H
