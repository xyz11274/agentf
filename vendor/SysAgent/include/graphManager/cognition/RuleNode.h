// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_RULENODE_H
#define SYSAGENT_RULENODE_H

#include <exception>
#include <iostream>
#include <ostream>
#include "aisf_platform/common_define.h"
#include "graphManager/GraphData.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/base/BaseNode.h"
#include "mm_node.h"
#include "ruleCognition/RuleHandlerManager.h"
#include "utils/http_client.h"
#include "utils/logger.h"
#include "utils/json_coder.h"
#include "callBack/callBack.h"
using namespace SYSAGENT;
class RuleNode : public BaseNode {
public:
    void HandleEmptySkill(std::shared_ptr<mmagentflow::MMMessage> msg, shared_ptr<Rule> rule) {
        auto accessCtrl = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
        TEHelper::sendUserOutputMsg(accessCtrl, "未找到相应的技能，请稍后重试", UserOutput_Completed, 0);
        TEHelper::sendNewTaskStatusReport(accessCtrl);
        rule = make_shared<Rule>();
        rule->ruleName = RuleName::RAG_RULE_HANDLER;
        rule->nextNode = GRAPH_NODE_EndNode;
    }

    RuleNode(const std::string &nodeName, const mmagentflow::MMNodeConfig &nodeConfig) : BaseNode(nodeName,
                                                                                                  nodeConfig) {
    }

    std::shared_ptr<mmagentflow::MMNode> Clone() { return std::make_shared<RuleNode>(name_, config_); }

    /**
     * @param msg
     * @return
     */
    mmagentflow::MMNodeFunctionStatus Run(std::shared_ptr<mmagentflow::MMMessage> msg)
    {
        CHECK_NULL_LOG_ERROR_RETURN(msg, mmagentflow::MMNodeFunctionStatus::FUNCTION_ERROR, "RuleNode msg is nullptr");
        LOG_INFO_EX("RequestId: %d, Run: %s", msg->GetRequestId(), name_.c_str());
        GraphUtils::AddDebugInfo(msg, name_);
        auto acc = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
        QueueManager::getInstance().EventMark("info", "RuleNode.enter", "success", "enter RuleNode run", acc);
        QueueManager::getInstance().EventMark("info", "RuleNode.doChainRuleCheckBegin", "success",
                                              "RuleNode ruleHandler start to doChain", acc);
        shared_ptr<Rule> rule = RuleHandlerManager::getInstance().doChain(acc);
        QueueManager::getInstance().EventMark("info", "RuleNode.doChainRuleCheckEnd", "success",
                                              "RuleNode ruleHandler finished doChain", acc);
        // 检查用户输入是否以"通通帮帮忙"开头
        if ((acc->taskAssignment.data.userInputText.rfind("通通帮帮忙", 0) == 0) ||
            (acc->taskAssignment.data.userInputText.rfind("通通帮我做", 0) == 0)) {
            // 从用户输入中提取关键词
            std::string keyword = extractKeywordFromInput(acc);
            if (!keyword.empty()) {
                // 调用KMM API获取技能信息
                bool success = recallSkill(acc, keyword);
                if (!success) {
                    LOG_ERR_EX("Failed to recall skill for keyword: %s", keyword.c_str());
                    HandleEmptySkill(msg, rule);
                    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
                }
                acc->taskAssignment.data.userIntent = "1";
                acc->taskAssignment.data.userSpecifiedAgent = "4";
                LOG_INFO_EX("User input starts with '通通帮帮忙/帮我做', set userintent=1, userspecifiedagent=4");
            }
        }
        if (acc->taskAssignment.data.userInputText.rfind("开始录制", 0) == 0) {
            acc->taskAssignment.data.userIntent = "2";
            acc->taskAssignment.data.userSpecifiedAgent = "4";
            LOG_INFO_EX("User input starts with '开始录制', set userintent=2, userspecifiedagent=4");
        }
        if (acc->userInput.data.userInputText.rfind("结束录制", 0) == 0) {
            acc->userInput.data.userInputObject = BuildEndRecordingUserInputObject();
            LOG_INFO_EX("User input starts with '结束录制', set userInputObject={\"code\":210003}");
        }
        // 指定意图直接走的规划节点
        if (rule->nextNode == GRAPH_NODE_PlanNode) {
            GraphUtils::AddMessageData(msg, GRAPH_DATA_PLANEXAMPLE,
                                       PlanExample(name_, rule->intentTasks, rule->agentDatas));  // 指定下一个节点
        }
        LOG_INFO_EX("current is [RuleNode], next node is [%s]", rule->nextNode.c_str());
        // 节点名称 常量
        GraphUtils::AddMessageData(msg, GRAPH_DATA_NEXTNODE, NextNode{{rule->nextNode}});  // 指定下一个节点
        GraphUtils::PrintDebugInfo(msg);
        QueueManager::getInstance().EventMark("info", "RuleNode.exit", "success", "exit RuleNode run", acc);
        return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
    }

    /**
     *
     * @param requestId 处理请求id
     * @param event 节点请求id的异步事件
     * @return
     */
    mmagentflow::MMNodeFunctionStatus AsyncRun(std::shared_ptr<mmagentflow::MMEvent> event)
    {
        return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
    }

    /**
     *
     * @param msg
     * @return
     */
    mmagentflow::MMNodeFunctionStatus CancelAsyncRun(std::shared_ptr<mmagentflow::MMMessage> msg)
    {
        return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
    }

    /**
     * 打断（cancel）场景只有在等待RunAsync存在，不存在在Run方法场景。
     * 增加异常
     * @param globalDatas
     * @return
     */
    std::shared_ptr<mmagentflow::MMMessage> DataMsgMerge(
            const std::map<std::string, std::shared_ptr<mmagentflow::MMMessage>> &inputmsgs)
            {
        return inputmsgs.begin()->second;
    }

private:

    std::string BuildEndRecordingUserInputObject()
    {
        Json::Value userInputObj;
        userInputObj["code"] = 210003;

        Json::StreamWriterBuilder writerBuilder;
        writerBuilder["indentation"] = "";
        return Json::writeString(writerBuilder, userInputObj);
    }
    /**
     * 从用户输入中提取关键词（通通帮帮忙后面的内容）
     * @param acc AccessCtrl对象
     * @return 提取的关键词
     */
    std::string extractKeywordFromInput(const AccessCtrlPtr &acc)
    {
        // 从TaskAssignment中获取用户输入
        std::string userInputText = acc->taskAssignment.data.userInputText;
        if (userInputText.empty()) {
            return "";
        }

        LOG_INFO_EX("User input text: %s", userInputText.c_str());

        // 使用正则表达式提取"通通帮帮忙"或"通通帮我做"后面的内容
        std::regex pattern("(?:通通帮帮忙|通通帮我做)(.*)");
        std::smatch match;
        if (std::regex_search(userInputText, match, pattern) && match.size() > 1) {
            std::string keyword = match[1].str();
            // 去除前后空格
            keyword.erase(0, keyword.find_first_not_of(" \t\n\r\f\v"));
            keyword.erase(keyword.find_last_not_of(" \t\n\r\f\v") + 1);
            LOG_INFO_EX("Extracted keyword: %s", keyword.c_str());
            return keyword;
        }
        return "";
    }

    /**
     * 调用KMM API获取技能信息并组装消息（同步版本）
     * @param acc AccessCtrl对象
     * @param keyword 搜索关键词
     * @return bool 操作是否成功
     */
// 辅助函数：处理KMM API响应的回调
// 辅助函数：从JSON中提取技能数据
    static bool ExtractSkillData(const Json::Value &skillJson,
                                 std::string &skillId,
                                 std::string &skillName,
                                 std::string &steps,
                                 std::string &summary,
                                 std::string &app,
                                 std::string &variables)
                                 {
        if (!skillJson.isObject()) {
            return false;
        }

        skillId = skillJson.get("skill_id", "").asString();
        skillName = skillJson.get("skill_name", "").asString();
        steps = skillJson.get("steps", "").asString();
        summary = skillJson.get("summary", "").asString();
        app = skillJson.get("app", "").asString();
        variables = skillJson.get("variables", "").asString();

        LOG_INFO_EX("Extracted skill info - ID: %s, Name: %s, App: %s",
                    skillId.c_str(), skillName.c_str(), app.c_str());
        return true;
    }

// 辅助函数：组装UserInputObject
    static std::string BuildUserInputObject(const std::string &app,
                                            const std::string &summary,
                                            const std::string &steps)
                                            {
        Json::Value userInputObj;
        userInputObj["code"] = 210005;
        userInputObj["data"]["app"] = app;
        userInputObj["data"]["overall_objective"] = summary;
        userInputObj["data"]["task_plan"] = steps;

        return JsonParser::ToRawString(userInputObj);
    }

// 修改后的回调处理函数
    static void HandleKmmApiCallback(
            const std::shared_ptr<CMFrm::COM::Http2ClientResponse> &http2Response,
            void *arg, std::promise<bool> &promise, const AccessCtrlPtr &acc, const std::string &keyword)
            {
        if (!http2Response) {
            LOG_ERR_EX("Http2Response is null in recallSkill callback");
            promise.set_value(false);
            return;
        }
        std::string bodyString = http2Response->GetResponseBody();
        LOG_INFO_EX("KMM API response: %s", bodyString.c_str());
        if (!http2Response->Is2XXStatusCode()) {
            LOG_ERR_EX("KMM API returned error status code: %d, response body: %s.",
                       http2Response->GetStatusCode(), bodyString.c_str());
            promise.set_value(false);
            return;
        }
        Json::Value jsonResponse;
        if (!JsonCoder::Parse(bodyString.c_str(), jsonResponse)) {
            LOG_ERR_EX("Failed to parse KMM API response: %s", bodyString.c_str());
            promise.set_value(false);
            return;
        }
        int retCode = jsonResponse.get("RetCode", 0).asInt();
        std::string retMsg = jsonResponse.get("RetMsg", "").asString();
        if (retCode != 200) {
            LOG_ERR_EX("KMM API returned error code: %d, message: %s", retCode, retMsg.c_str());
            promise.set_value(false);
            return;
        }
        const Json::Value &skills = jsonResponse["skills"];
        if (!skills.isArray() || skills.size() == 0) {
            LOG_ERR_EX("No skills found in KMM response");
            promise.set_value(false);
            return;
        }
        // 使用第一个技能数据
        std::string skillId; std::string skillName; std::string steps;
        std::string summary; std::string app; std::string variables;
        if (!ExtractSkillData(skills[0], skillId, skillName, steps, summary, app, variables)) {
            promise.set_value(false);
            return;
        }
        std::string userInputObject = BuildUserInputObject(app, summary, steps); // 组装UserInputObject
        acc->taskAssignment.data.userInputText = "通通帮帮忙" + keyword; // 设置AccessCtrl中的TaskAssignment数据
        acc->taskAssignment.data.userInputObject = userInputObject;
        acc->taskAssignment.header.messageType = "UserTaskManagement";
        LOG_INFO_EX("Successfully set user input object in AccessCtrl");
        promise.set_value(true);  // 成功完成
    }

// 修改后的recallSkill函数
    static bool recallSkill(const AccessCtrlPtr &acc, const std::string &keyword) {
        UserInfo userInfo = TEHelper::extractUserInfo(acc);
        std::string userId = userInfo.userName;
        if (userId.empty()) {
            LOG_ERR_EX("userId is empty in recallSkill");
            return false;
        }

        LOG_INFO_EX(" userId: %s, keyword: %s", userId.c_str(), keyword.c_str());
        std::string endpointIP = HttpClient::GetServiceIP("KMMService");
        std::string url = "https://" + endpointIP + ":27203" + URL_STEP_TEACH_SKILL.data() + "?keyWord=" + keyword;
        std::map<std::string, std::string> headers{};
        headers.insert(std::make_pair("userId", userId));
        LOG_INFO_EX("Calling KMM API: %s", url.c_str());

        std::promise<bool> promise;
        std::future<bool> future = promise.get_future();

        auto retCode = HttpClient::Http2SendRequest(
                url, headers,
                [acc, keyword, &promise](
                        const auto &http2Response, void *arg) {
                    HandleKmmApiCallback(http2Response, arg, promise, acc, keyword);
                }, 5);

        if (!retCode) {
            LOG_ERR_EX("Failed to send Http2 request for recallSkill");
            return false;
        }

        auto status = future.wait_for(std::chrono::seconds(5));
        if (status == std::future_status::ready) {
            return future.get();
        } else {
            LOG_ERR_EX("recallSkill timeout after 5 seconds");
            return false;
        }
    }
};
#endif  // SYSAGENT_RULENODE_H