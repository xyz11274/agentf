//
// Created by c00888021 on 2025/12/19.
//

#include <model/ModelInfer.h>
#include "config/Config.h"
#include "model/PromptManager.h"
#include "model/ReactLLMTask.h"
#include "utils/env_util.h"
#include "utils/curl_client.h"

void ModelInfer::ModelInferWithContext(const std::string& modelName, const std::string& promptName,
                                       const std::vector<std::map<std::string, std::string>> memories,
                                       std::shared_ptr<std::promise<std::string>> promise, std::string userPrompt)
{
    std::string modelServiceName = CMFrm::Utils::GetEnv("INTENT_CONCLUDE_SERVICE_NAME", "llmlargeservice");
    mmsdk::MMReasonParam param = {
        .modelServiceName = modelServiceName,
        .modelName = "Qwen3-32B",
        .maxToken = 1024
    };
    userPrompt += "/nothink";
    auto intention = mmsdk::MMUserIntention{.query = userPrompt,
            .modaltype = mmsdk::MMReasonModalType::MMLLM_MODAL_TYPE_TEXT};
    auto req = mmsdk::MMLLMRequest{
            .intentions = {intention},
            .prompt = {PromptManager::GetInstance().GetPormptTmpl(promptName)}};
    auto task = std::make_shared<ReactLLMTask>();
    task->RegisterCallback(LLMEventType::OnLLMFinalAnswer, [promise](const LLMEventData& data) {
        auto answer = std::get<OnLLMFinalAnswerEvent>(data);
        LOG_INFO_EX("ModelInferWithContext result is:%s", answer.answer.c_str());
        CHECK_NULL_LOG_ERROR_RETURN_VOID(promise, "promise is null");
        promise->set_value(answer.answer);
    });
    task->RegisterCallback(LLMEventType::OnLLMError, [promise](const LLMEventData& data) {
        LOG_ERR_EX("ModelInferWithContext failed");
        CHECK_NULL_LOG_ERROR_RETURN_VOID(promise, "promise is null");
        promise->set_value(std::string(""));
    });
    reactLLMomponent->CreateTask(
            param, task,
            [req, memories](uint32_t errorCode, std::string& taskId) -> uint32_t {
                CustomedLLMComponent->UpdateUserContext(taskId, memories,[req, taskId](uint32_t errorCode) -> uint32_t {
                    auto ret = reactLLMomponent->RequestAsync(taskId, req, {});
                    LOG_INFO_EX("RequestAsyncInvoke result is:%d", ret);
                    return 0;
                });
                return 0;
            });
}

Json::Value CreateMessage(const std::string& role, const std::string& content)
{
    Json::Value message;
    message["role"] = role;
    message["content"] = content;
    return message;
}

std::string FormatModelBody(const std::string& sysPrompt, const Json::Value& memories,
                            const std::string& userPrompt, const std::string& modelName)
{
    Json::Value sys = CreateMessage("system", sysPrompt);
    Json::Value user = CreateMessage("user", userPrompt);

    Json::Value messages;
    messages.append(sys);
    if (!memories.isArray()) {
        for (const auto& memory : memories) {
            if (memory.isMember("originalQA") && memory["originalQA"].isArray()) {
                for (auto& originalQA : memory["originalQA"]) {
                    std::map<std::string, std::string> qaPair;
                    JsonParser::GetString(originalQA, "role", qaPair["role"]);
                    JsonParser::GetString(originalQA, "content", qaPair["content"]);
                    messages.append(CreateMessage(qaPair["role"], qaPair["content"]));
                }
            }
        }
    }
    messages.append(user);

    Json::Value body;
    body["model"] = modelName;
    body["messages"] = messages;
    body["temperature"] = 0.0;
    body["max_tokens"] = CMFrm::Constants::Const2048;
    body["enable_thinking"] = false;
    return JsonParser::ToRawString(body);
}

void ModelInfer::ModelInferWithContextThird(const std::string& modelName, const std::string& promptName,
                                            const Json::Value& memories,
                                            std::shared_ptr<std::promise<std::string>> promise,
                                            const std::string& userPrompt)
{
    auto res = std::async(std::launch::async, [=]() {
        std::string url = ConfigManager::getInstance().getEnvInfo("OUTER_LLM_MODEL_URL", "");
        std::string apiKey = ConfigManager::getInstance().getEnvInfo("OUTER_LLM_MODEL_API_KEY", "");
        LOG_INFO_EX("Outer model's apiKey is:%s", apiKey.c_str());
        auto sysPrompt = PromptManager::GetInstance().GetPormptTmpl(promptName);
        std::string body = FormatModelBody(sysPrompt.GetContent(), memories, userPrompt, modelName);
        std::map<std::string, std::string> headers {{"Authorization", "Bearer " + apiKey}};

        CurlClient client;
        client.AddHeader("Content-Type: application/json");
        client.AddHeader("Authorization: Bearer " + apiKey);
        auto ip = SYSAGENT::HttpClient::GetServiceIP("MMGateWay");
        CHECK_TRUE_LOG_ERROR_RETURN_VOID(ip.empty(), "mmagetay ip is empty");
        LOG_INFO_EX("summary, mmgatewat ip is: %s", ip.c_str());
        client.SetSocks5Proxy(ip, 27262);
        std::string result = client.Post(url, body);
        if (!result.empty()) {
            Json::Value root;
            JsonParser::Parse(result, root);
            if (root.isMember("choices") && root["choices"].isArray()) {
                auto data = root["choices"][0];
                if (data.isMember("message") && data["message"].isObject() && data["message"].isMember("content")) {
                    std::string answer = "";
                    JsonParser::GetString(data["message"], "content", answer);
                    LOG_INFO_EX("siliconflow response is :%s", answer.c_str());
                    promise->set_value(answer);
                }
            }
        }
    });
}
