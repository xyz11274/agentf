//
// Created by c00888021 on 2026/1/4.
//

#ifndef SYSAGENT_MODELINFER_H
#define SYSAGENT_MODELINFER_H

#include <string>
#include <map>
#include <future>
#include <vector>
#include <json/json.h>

namespace ModelInfer {

    void ModelInferWithContext(const std::string& modelName, const std::string& promptName,
                               const std::vector<std::map<std::string, std::string>> memories,
                               std::shared_ptr<std::promise<std::string>> promise, std::string userPrompt);

    void ModelInferWithContextThird(const std::string& modelName, const std::string& promptName,
                                    const Json::Value& memories, std::shared_ptr<std::promise<std::string>> promise,
                                    const std::string& userPrompt);
}

#endif //SYSAGENT_MODELINFER_H
