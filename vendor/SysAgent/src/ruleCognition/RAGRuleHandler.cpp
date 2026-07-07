//
// Created by z00913804 on 25-7-28.
//

#include "framework_common/utils/env_util.h"

#include "ruleCognition/RAGRuleHandler.h"
#include "TEManager.h"
#include "TeHelper.h"
#include "graphManager/GraphData.h"
#include "memory/SafetyFence.h"
#include "queue/QueueManager.h"
#include "ruleCognition/Rule.h"
#include "ruleCognition/CommonDefinition.h"
#include "config/Config.h"

shared_ptr<Rule> RAGRuleHandler::doChain(const std::shared_ptr<AccessCtrl>& accessCtrl)
{
    LOG_INFO_EX("RAGRuleHandler enter");
    if (accessCtrl == nullptr) {
        LOG_INFO_EX("[RAGRuleHandler] accessCtrl nullptr");
        return nullptr;  // ragHandler通过
    }
    // 获取环境变量里面的开关
    std::string ragSafetyFenceFlag = ConfigManager::getInstance().getEnvInfo(ENV_RAG_SAFETYFENCE_FLAG, CLOSE);
    if (ragSafetyFenceFlag == CLOSE) {
        LOG_INFO("ragSafetyFenceFlag is false, close the safety check, pass !");
        return nullptr;
    }
    LOG_INFO_EX("ragSafetyFenceFlag : %s", ragSafetyFenceFlag.c_str());
    string taskID = accessCtrl->header.taskID;
    std::shared_ptr<TeamEnvironment> te = TEManager::getInstance().getTEById(accessCtrl->header.teamEnvironmentID);
    string request = accessCtrl->extractInput();
    if (request.empty()) {
        LOG_INFO_EX("input is null");
        return nullptr;
    }

    shared_ptr<Rule> rule;
    // rag检索不为空，说明请求非法，返回 1 ragHandler 执行成功， -1 继续下一个handler
    if (accessCtrl->eventType == EventType::TASKASSIGNMENT) {
        QueueManager::getInstance().EventMark("info", "RAGRuleHandler.SafetyFenceBegin", "success",
                                                      "SafetyFence SimilaritySearch", accessCtrl);
        auto result = SafetyFence::SimilaritySearch(request, {"query", "content"});
        QueueManager::getInstance().EventMark("info", "RAGRuleHandler.SafetyFenceEnd", "success",
                                              "SafetyFence SimilaritySearch end", accessCtrl);
        if (!result.empty()) {
            // 发送UserOutput报文
            std::string content = result[2];
            std::string status = UserOutput_Completed;
            std::string intentName = SafetyFence::parseIntentName(content);
            std::string dataContent = SafetyFence::parseRejectContent(content);
            LOG_INFO_EX("intentName [%s], dataContent [%s]", intentName.c_str(), dataContent.c_str());
            if (intentName == "REJECTED_ANSWER") { // 拒答
                TEHelper::sendUserOutputMsg(accessCtrl, dataContent, status, 0);
                TEHelper::sendNewTaskStatusReport(accessCtrl); // 斟酌一下，任务创建不合法的话是没法更新状态的；任务过程中应该是不需要结束任务的。
                rule = make_shared<Rule>();
                rule->ruleName = RuleName::RAG_RULE_HANDLER;  // 常量表，定义是这些handler
                rule->nextNode = GRAPH_NODE_EndNode;
            }
            return rule;
        }
    } else if (accessCtrl->eventType == EventType::USERINPUT) {
        QueueManager::getInstance().EventMark("info", "RAGRuleHandler.SafetyFenceBegin", "success",
                                      "SafetyFence SimilaritySearch", accessCtrl);
        auto result = SafetyFence::SimilaritySearch(request, {"query", "content"});
        QueueManager::getInstance().EventMark("info", "RAGRuleHandler.SafetyFenceEnd", "success",
                              "SafetyFence SimilaritySearch end", accessCtrl);
        if (!result.empty()) {
            // 发送UserOutput报文
            std::string content = result[2];
            std::string status = UserOutput_Completed;
            std::string intentName = SafetyFence::parseIntentName(content);
            std::string dataContent = SafetyFence::parseRejectContent(content);
            LOG_INFO_EX("intentName [%s], dataContent [%s]", intentName.c_str(), dataContent.c_str());
            if (intentName == "REJECTED_ANSWER") { // 拒答
                TEHelper::sendUserOutputMsg(accessCtrl, dataContent, status, 0);
                rule = make_shared<Rule>();
                rule->ruleName = RuleName::RAG_RULE_HANDLER;
                rule->nextNode = GRAPH_NODE_EndNode;
            }
            return rule;
        }
    }
    return rule;  // ragHandler通过，//要返回共享指针！
}
