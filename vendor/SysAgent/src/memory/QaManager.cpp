//
// Created by c00888021 on 2026/1/6.
//
#include "memory/QaManager.h"
#include "queue/QueueManager.h"

Json::Value GenerateQA(std::string query, std::string answer)
{
    Json::Value user;
    user["role"] = "user";
    user["content"] = query;
    Json::Value assistant;
    assistant["role"] = "assistant";
    assistant["content"] = answer;
    Json::Value qaArray;
    qaArray.append(user);
    qaArray.append(assistant);
    return qaArray;
}

std::tuple<Json::Value, std::string> GenerateAbstractQA(std::string response)
{
    Json::Value data;
    if (response.empty() || !AisfPlat::JsonParser::Parse(response, data)) {
        return std::make_tuple(Json::Value(), "");
    }
    std::string type = "";
    AisfPlat::JsonParser::GetString(data, "tag", type);
    if (data.isMember("abstract") && data["abstract"].isArray()) {
        return std::make_tuple(data["abstract"], type);
    }
    return std::make_tuple(Json::Value(), type);
}

void QAQueueManager::consumeFunc(int queue_idx)
{
    QaData data;
    while (true) {
        if (!queues_[queue_idx]->pop(data, stop_flag_)) { continue; }
        std::vector<QAEntity> list = std::get<0>(data);
        std::string userName = std::get<1>(data);
        std::string reWriteQ = std::get<2>(data);
        std::string uuid = std::get<3>(data);
        std::string taskID = std::get<4>(data);
        auto memories = MemoryMgr::GetRemoteMemories(userName, taskID, CMFrm::Constants::Const10);
        std::promise<string> promiseQA;
        auto qaPtr = std::make_shared<std::promise<std::string>>(std::move(promiseQA));
        std::promise<string> promiseAbstract;
        auto abstractPtr = std::make_shared<std::promise<std::string>>(std::move(promiseAbstract));
        if (list.size() < CMFrm::Constants::Const2) {
            LOG_INFO_EX("list size is Illegal");
            continue;
        }
        std::string query = list[0].message;
        std::string answer = list[list.size() - 1].message;
        std::ostringstream oss;
        oss << "##用户输入\n" << query << "\n##系统回复" << answer << "\n";
        std::string userPrompt = oss.str();
        LOG_INFO_EX("queue_idx:%d, Generate QA Abstract user input is:%s", queue_idx, userPrompt.c_str());
        auto accessCtrl = std::make_shared<AccessCtrl>();
        accessCtrl->header.userName = userName;
        accessCtrl->header.taskID = taskID;
        QueueManager::getInstance().EventMark("info", "GenerateQA.Enter", "success", "Generate QA run", accessCtrl);
        QueueManager::getInstance().EventMark("info", "GenerateAbstract.Enter", "success", "Generate Abstract run",
                                              accessCtrl);
        ModelInfer::ModelInferWithContextThird("Qwen/Qwen3-32B", "generate_abstract_qa", memories, qaPtr, userPrompt);
        ModelInfer::ModelInferWithContextThird("Qwen/Qwen3-32B", "generate_abstract_content", memories, abstractPtr,
                                               userPrompt);
        std::string responseQA = "";
        std::string responseAbstract = "";
        SYSAGENT::CommonFunc::WaitForPromise(qaPtr, responseQA, "GenerateQA.Exit", accessCtrl);
        SYSAGENT::CommonFunc::WaitForPromise(abstractPtr, responseAbstract, "GenerateAbstract.Exit", accessCtrl);
        auto qaAbstract = GenerateAbstractQA(responseQA);
        QAObject qaObject{
                .originalQA = GenerateQA(query, answer), .abstractQA = std::get<0>(qaAbstract),
                .abstract = responseAbstract, .rewriteQ = reWriteQ, .outType = std::get<1>(qaAbstract),
                .keys = "", .userId = userName
        };
        auto ptr = std::make_shared<QAObject>(qaObject);
        ptr->id = uuid;
        MemoryMgr::AddShortMemories("", "update", ptr, [](bool isSuccess, std::string reponse) {
            LOG_INFO_EX("response is:%s", reponse.c_str());
        });
    }
}