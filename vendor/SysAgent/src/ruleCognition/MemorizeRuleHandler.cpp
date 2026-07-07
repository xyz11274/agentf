//
// Created by c00888021 on 2025/8/25.
//
#include "ruleCognition/MemorizeRuleHandler.h"
#include "memory/History.h"
#include "TeHelper.h"
#include "graphManager/GraphData.h"
#include "queue/QueueManager.h"
#include "memory/ContextManager.h"

shared_ptr<Rule> MemorizeRuleHandler::doChain(const std::shared_ptr<AccessCtrl> &accessCtrl)
{
    LOG_INFO_EX("MemorizeRuleHandler enter");
    CHECK_NULL_LOG_ERROR_RETURN(accessCtrl, nullptr, "[MemorizeRuleHandler] accessCtrl nullptr");
    std::string request = accessCtrl->extractInput();
    CHECK_TRUE_LOG_ERROR_RETURN(request.empty(), nullptr, "input is null");

    shared_ptr<Rule> rule;
    if (request.find(CONTEXT_CLEAR) != std::string::npos) {
        UserInfo userInfo = TEHelper::extractUserInfo(accessCtrl);
        ContextManager::DelContext(userInfo.userName, [=](bool isSuccess) {
            LOG_INFO_EX("clear context result is:%d", isSuccess);
        });
        std::string response = accessCtrl->extractSender() == "LTServer" ? "" : CONTEXT_CLEAR_FINISHED;
        TEHelper::sendUserOutputMsg(accessCtrl, response, UserOutput_Completed, 0);
        TEHelper::sendUserTaskCompleted(accessCtrl);
        rule = make_shared<Rule>();
        rule->ruleName = RuleName::MEMORIZE_RULE_HANDLER;
        rule->nextNode = GRAPH_NODE_EndNode;
        return rule;
    }

    std::string opType = "";
    std::string content = "";
    std::vector<std::string> remember = {"帮我记一下", "帮我更新一下", "帮我记住"};
    std::string matchRemember = "";
    for (auto rememberStr : remember) {
        if (request.find(rememberStr) == 0) {
            matchRemember = rememberStr;
        }
    }
    if (!matchRemember.empty()) {
        opType = "ADD";
        content = request.substr(request.find(matchRemember) + std::string(matchRemember).size());
    } else if (request.find(HELP_ME_FORGET) == 0) {
        opType = "DELETE";
        content = request.substr(request.find(HELP_ME_FORGET) + std::string(HELP_ME_FORGET).size());
    } else if (request.find(HELP_ME_QUERY) != std::string::npos) { // 查询记住的记忆信息
        queryMemory(accessCtrl);
        rule = make_shared<Rule>();
        rule->ruleName = RuleName::MEMORIZE_RULE_HANDLER;
        rule->nextNode = GRAPH_NODE_EndNode;
    }
    auto te = TEManager::getInstance().getTEById(accessCtrl->header.teamEnvironmentID);
    CHECK_NULL_LOG_ERROR_RETURN(te, rule, "GET te failed");
    if (!opType.empty()) {
        MemorizeMessage msg = {
            .metadata = "",
            .opType = opType,
            .content = content,
        };
        CHECK_NULL_LOG_ERROR_RETURN(te, nullptr, "GET te failed");
        std::string userId = te->getUserInfo().userName;
        std::string url(URL_MEMORY_UPDATE);
        UpdateMemory(userId, msg, url, accessCtrl->header.taskID);
        TEHelper::sendUserOutputMsg(accessCtrl, MEMORIZE_UPDATE, UserOutput_Completed, 0);
        sendTaskStatusReport(accessCtrl, te);
        AddContextWithoutTask(accessCtrl, accessCtrl->extractInput(), MEMORIZE_UPDATE, te->getUserInfo().userName);
        rule = make_shared<Rule>();
        rule->ruleName = RuleName::MEMORIZE_RULE_HANDLER;
        rule->nextNode = GRAPH_NODE_EndNode;
    }
    return rule;
}

void MemorizeRuleHandler::queryMemory(const std::shared_ptr<AccessCtrl>& accessCtrl)
{
    std::string endpointIP = SYSAGENT::HttpClient::GetServiceIP("KMMService");
    if (endpointIP.empty()) {
        LOG_ERR_EX("[MEMORY] get kmmService instance empty");
        return;
    }

    auto http2Client = CMFrm::COM::HttpClientFactory::GetHttpClientFactory()->GetHttp2Client("KMMService");
    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    req->SetMethod(CMFrm::COM::HttpMethod::POST);
    req->SetReqUrl("https://" + endpointIP + ":27203" + URL_MEMORY_QUERY.data());
    req->SetBody("{}");
    LOG_INFO_EX("request url is:%s", req->GetReqUrl().c_str());
    auto te = TEManager::getInstance().getTEById(accessCtrl->header.teamEnvironmentID);
    CHECK_NULL_LOG_ERROR_RETURN(te, void(), "GET te failed");
    req->AddHeader("userId", te->getUserInfo().userName);
    req->SetTimeoutSecond(NUMBER_TEN);
    http2Client->SendAsync(req, [=](std::shared_ptr<CMFrm::COM::Http2ClientResponse> resp, void*) {
        LOG_INFO_EX("query result: %d, %s", resp->GetStatusCode(), resp->GetResponseBody().c_str());
        if (resp->GetStatusCode() == CMFrm::COM::HTTP200) {
            // 解析结果
            Json::Value data;
            JsonParser::Parse(resp->GetResponseBody(), data);
            if (data.isMember("data") && data["data"].isArray()) {
                std::string result = "";
                int index = 1;
                for (auto& item : data["data"]) {
                    if (item.isMember("content") && item["content"].isString()) {
                        result += std::to_string(index) + ". " + item["content"].asString() + "\n";
                    }
                    result += index == data.size() - 1 ? "" : "\n";
                    index ++;
                }
                if (result.empty()) {
                    result = "非常抱歉，暂时无法获取到您的相关记忆。";
                }
                TEHelper::sendUserOutputMsg(accessCtrl, result, UserOutput_Completed, 0);
                AddContextWithoutTask(accessCtrl, accessCtrl->extractInput(), result, te->getUserInfo().userName);
            }
            sendTaskStatusReport(accessCtrl, te);
        }
    });
}

void MemorizeRuleHandler::sendTaskStatusReport(const std::shared_ptr<AccessCtrl>& accessCtrl,
                                               shared_ptr<TeamEnvironment> te)
{
    CHECK_NULL_LOG_ERROR_RETURN_VOID(te, "te is nullptr");
    auto statusReport = TEHelper::newTaskStatusReport(te->getTeamId(), 200, TaskStatusReport_TaskCompleted);
    CHECK_NULL_LOG_ERROR_RETURN_VOID(statusReport, "statusReport is nullptr");
    statusReport->setSender("SystemAgent");
    statusReport->setReceiver(accessCtrl->extractSender());
    statusReport->setClientExField(accessCtrl->taskAssignment.header.clientExField);
    QueueManager::getInstance().postWriteQueue(statusReport, te->session);
}

void MemorizeRuleHandler::AddContextWithoutTask(const std::shared_ptr<AccessCtrl>& accessCtrl, std::string query,
                                                std::string answer, std::string userId)
{
    LOG_INFO_EX("standardized phrasing add context, query:%s, answer:%s", query.c_str(), answer.c_str());
    Json::Value data;
    Json::Value item;
    item["query"] = query;
    item["answer"] = answer;
    item["custom_summary"] = "";
    item["re_write_query"] = "";
    item["date"] = AisfPlat::Utils::GetCurrentTimeMillisecondStr();
    data.append(item);

    ContextManager::AddContext(userId, "", "", "", "", data, [](bool isSuccess, std::vector<std::string>) {
        LOG_INFO_EX("standardized phrasing result:%d", isSuccess);
    });
}
