/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: This file implements for callback.cpp
 */

#ifndef SYSAGENT_QAAGENTCALLBACK_H
#define SYSAGENT_QAAGENTCALLBACK_H

using namespace mmsdk;

#include "TEManager.h"
#include "TeHelper.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "aisf_platform/parser/json_parser.h"
#include "memory/History.h"
#include "mm_customedreact_llm_component.h"
#include "mm_error_code_ext.h"
#include "queue/QueueManager.h"
#include <map>
#include <string>

class QAAgentCallback : public MMLLMAsyncCallBack {
public:
    uint32_t OnLLMStart(const std::string& taskId, const std::string& reqId) override
    {
        std::cout << "[OnLlmStreamStart] sessionId: " + taskId << std::endl;
        auto te = TEManager::getInstance().getTEById(teid_);
        if (te != nullptr) {
            think_flag_ = te->thinkFlag;
        }
        return 0;
    }

    uint32_t OnLLMEnd(const std::string& taskId, const std::string& reqId) override
    {
        std::cout << "[OnLlmStreamEnd] sessionId: " + taskId << std::endl;
        return 0;
    }

    // 1、接收到大模型的返回结果
    uint32_t OnLLMNewToken(const std::string& taskId, const std::string& reqId, const std::string& token) override
    {
        buffer += token;
        auto current_time = std::chrono::steady_clock::now();
        auto duration_since_last_send =
            std::chrono::duration_cast<std::chrono::milliseconds>(current_time - last_send_time);
        // TODO：缓冲一下
        if (first_token_ || buffer.size() > 500 || duration_since_last_send.count() > 500 || token == "</think>") {
            std::cout << token << std::endl;
            std::string userOutputText;
            if (think_flag_) {
                userOutputText = "<font color=\"gray\" size=\"12\">" + buffer + "</font>";
            } else {
                userOutputText = buffer;
            }

            LOG_INFO_EX("buffer: %s", userOutputText.c_str());

            if (!SendUserOutput(userOutputText, "Running", 0)) {
                LOG_INFO_EX("send tocken failed");
                return 1;
            }
            last_send_time = current_time;  // 更新上次发送时间
            buffer.clear();  // 清空缓存

            if (token == "</think>") {
                LOG_ERR_EX("get </think>, taskId: %s", taskId.c_str());
                think_flag_ = false;
            }
            first_token_ = false;
        }
        return 0;
    }

    std::string formatTimeString(const std::string& input)
    {
        std::string result = input;
        // 替换 'T' 为一个空格
        for (char& c : result) {
            if (c == 'T') {
                c = ' ';
            }
        }
        // 删除末尾的 'Z'
        if (!result.empty() && result.back() == 'Z') {
            result.pop_back();
        }
        return result;
    }

    // 结果汇总
    uint32_t OnLLMFinalAnswer(const std::string& taskId, const std::string& reqId, const std::string& answer) override
    {
        first_token_ = true;
        auto te = TEManager::getInstance().getTEById(teid_);
        if (!te) {
            LOG_ERR_EX("[newUserOutput] TeamEnvironment with ID %s not found", teid_.c_str());
            return 1;
        }

        if (buffer.size() >= 0) {
            LOG_INFO_EX("[OnLLMFinalAnswer]send buffer to system agent start, buffer:%s", buffer.c_str());
            std::cout << buffer << std::endl;
            if (!SendUserOutput(buffer, "Completed", 0)) {
                LOG_ERR_EX("[OnLLMFinalAnswer]send tocken failed");
                return 1;
            }
            auto current_time = std::chrono::steady_clock::now();
            last_send_time = current_time;  // 更新上次发送时间
            buffer.clear();  // 清空缓存
        } else {
            if (!SendUserOutput("", "Completed", 0)) {
                LOG_ERR_EX("[OnLlmFinalAnswer]send token failed");
            }
            LOG_ERR_EX("[OnLlmFinalAnswer] taskId: %s, finalAnswer:%s", taskId.c_str(), answer.c_str());
            std::cout << "[OnLlmFinalAnswer] taskId: " + taskId + ", finalAnswer: " + answer << std::endl;
        }

        // add memory without think
        size_t think_pos = answer.find("</think>");
        std::string result = "";
        if (think_pos == std::string::npos) {
            std::cout << "[OnLlmFinalAnswer] no think: " + taskId + ", finalAnswer: " + answer << std::endl;
            result = answer;
        } else {
            result = answer.substr(think_pos + 8, answer.size());
        }
        LOG_ERR_EX("[OnLlmFinalAnswer] taskId: %s, finalAnswer:%s", taskId.c_str(), answer.c_str());
        if (!SendTaskStatusReport(CMFrm::COM::HttpStatus::HTTP200, "TaskCompleted")) {
            LOG_ERR_EX("[OnLlmFinalAnswer]send end msg to sysagent failed");
        }
        std::string workFlowID = "";
        if (te->getWorkFlow()) {
            workFlowID = te->getWorkFlow()->GetWorkflowID();
        }
        Memory memory {
            .userId = te->getUserInfo().userName,
            .sessionId = workFlowID,
            .role = "system",
            .content = result,
            .date = formatTimeString(AisfPlat::Utils::GetCurrentTimestampInCustomFormat())
        };
        QueueManager::getInstance().postKmmMsgReadQueue(memory);

        return 0;
    }

    uint32_t OnLLMError(const std::string& taskId, const std::string& reqId, uint32_t resultCode) override
    {
        first_token_ = true;
        std::cout << "[OnLlmError] sessionId: " + taskId + ", errorCode: " << resultCode << std::endl;
        LOG_ERR_EX("taskId: %s, reqId: %s, code: %d", taskId.c_str(), reqId.c_str(), resultCode);
        if (resultCode == mmsdk::MMMasterErrCode::RT_INVALID_TASKID || resultCode == NUMBER_SEVEN
        || resultCode == mmsdk::MMMasterErrCode::RT_UNIX_URL_ERROR || resultCode == NUMBER_FIVE) {
            LOG_ERR_EX("taskId: %s, not exist", taskId.c_str());
            auto te = TEManager::getInstance().getTEById(teid_);
            if (!te) {
                LOG_ERR_EX("Failed to get te");
                return false;
            }
            LOG_ERR_EX("reset qa task");
            te->setQATask("-1");
        }

        if (!SendUserOutput("大模型调用失败", "Completed", 0)) {
            LOG_ERR_EX("[OnLlmError] send output failed");
            return 1;
        }
        if (!SendTaskStatusReport(CMFrm::COM::HttpStatus::HTTP200, "TaskCompleted")) {
            LOG_ERR_EX("[OnLlmError] send status failed");
        }
        return 0;
    }

    uint32_t OnToolCallName(const std::string& taskId, const std::string& reqId, const std::string& toolName) override
    {
        std::cout << "[OnToolCallWill] taskId: " + taskId + ", toolName: " + toolName << std::endl;
        return 0;
    }

    bool OnToolConfirm(const std::string& taskId, const std::string& reqId, const std::string& toolName,
                       const std::map<std::string, std::string>& toolArgs) override
    {
        return true;
    }

    uint32_t OnToolStart(const std::string& taskId, const std::string& reqId, const std::string& toolName,
                         const std::map<std::string, std::string>& toolArgs) override
    {
        std::string toolArgsStr;
        for (const auto& pair : toolArgs) {
            toolArgsStr += pair.first + ": " + pair.second + "\n";
        }

        std::string toolNameZN = toolName;
        for (const auto& tool : ConfigManager::getInstance().getToolList()) {
            if (tool.id == toolName) {
                toolNameZN = tool.name;
            }
        }

        std::string userOutputText;
        auto te = TEManager::getInstance().getTEById(teid_);
        if (te != nullptr) {
            std::string toolTips = "通通正在进行联网搜索。";
            userOutputText = te->thinkFlag ? "<font color=\"gray\" size=\"12\">" + toolTips + "</font>" : toolTips;
            LOG_INFO("tool name is:%s", toolNameZN.c_str());
        }

        LOG_INFO_EX("userOutputText:%s", userOutputText.c_str());
        if (!SendUserOutput(userOutputText, "Running", 0)) {
            LOG_ERR_EX("send tocken failed");
            return 1;
        }

        return 0;
    }

    uint32_t OnToolEnd(const std::string& taskId, const std::string& reqId, const std::string& toolName,
                       const std::map<std::string, std::string>& toolArgs, const std::string& toolResponse) override
    {
        std::string toolArgsStr;
        for (const auto& pair : toolArgs) {
            toolArgsStr += pair.first + ": " + pair.second + "\n";
        }
        LOG_INFO_EX("[OnToolEnd] taskId:%s, toolName:%s, toolArgsStr: %s, toolResponse:%s", taskId.c_str(),
                    toolName.c_str(), toolArgsStr.c_str(), toolResponse.c_str());
        std::cout << "[OnToolEnd] taskId: " + taskId + ", toolName: " + toolName + "toolArgsStr: " + toolArgsStr +
                         "toolResult" + toolResponse
                  << std::endl;
        std::string userOutputText;
        auto te = TEManager::getInstance().getTEById(teid_);
        if (te != nullptr) {
            userOutputText = te->thinkFlag ? "<font color=\"gray\" size=\"12\">接下来为您总结输出：</font>" :
                                             "接下来为您总结输出：";
        }

        LOG_INFO_EX("userOutputText:%s", userOutputText.c_str());

        if (!SendUserOutput(userOutputText, "Running", 0)) {
            LOG_ERR_EX("[OnToolEnd]send tocken failed");
            return 1;
        }
        return 0;
    }

    uint32_t OnToolError(const std::string& taskId, const std::string& reqId, const std::string& toolName,
                         const std::map<std::string, std::string>& toolArgs, const uint32_t resultCode) override
    {
        LOG_ERR_EX("[OnToolError] taskId:%s, toolinfo:%s, errorcode: %u", taskId.c_str(), toolName.c_str(), resultCode);
        std::cout << "[OnToolError] taskId: " + taskId + ", toolinfo: " + taskId + "error:" << resultCode << std::endl;

        std::string userOutputText;
        auto te = TEManager::getInstance().getTEById(teid_);
        if (te != nullptr) {
            userOutputText = te->thinkFlag ? "<font color=\"gray\" size=\"12\">" + std::string("工具执行失败: \n") +
                                                 toolName + "</font>" :
                                             "工具执行失败: \n" + toolName;
        }

        LOG_INFO_EX("userOutputText:%s", userOutputText.c_str());
        if (!SendUserOutput(userOutputText, "Completed", 0)) {
            LOG_ERR_EX("[OnToolError]send tocken failed");
            return 1;
        }

        if (!SendTaskStatusReport(CMFrm::COM::HttpStatus::HTTP200, "TaskCompleted")) {
            LOG_ERR_EX("[OnToolError]send end msg to sysagent failed");
        }
        return 0;
    }

    void setTeid(const std::string& teid) { teid_ = teid; }

    void setTaskid(const std::string& taskid) { taskid_ = taskid; }

private:
    // 发送UserOutput消息
    bool SendUserOutput(std::string output, std::string streamStatus, int outputFlag)
    {
        auto te = TEManager::getInstance().getTEById(teid_);
        if (!te) {
            LOG_ERR_EX("Failed to get te");
            return false;
        }
        auto wf = te->getWorkFlow(taskid_);
        if (!wf) {
            LOG_ERR_EX("Failed to get workflow");
            return false;
        }
        auto msg = TEHelper::newUserOutput(teid_, output, streamStatus, outputFlag);
        if (!msg) {
            LOG_ERR_EX("Failed to get te");
            return false;
        }
        msg->setSender("SystemAgent");
        msg->setReceiver(wf->GetTopSender());
        msg->setTaskID(taskid_);
        msg->setClientExField(wf->GetClientExField());

        QueueManager::getInstance().postWriteQueue(msg, te->session);

        return true;
    }

    // 发送TaskStatusReport消息
    bool SendTaskStatusReport(int statusCode, std::string statusReport)
    {
        LOG_INFO_EX("send TaskStatusReport to APP, statusCode: %d, statusReport: %s", statusCode, statusReport.c_str());
        std::cout << "send TaskStatusReport to APP, statusCode: " << statusCode << ", statusReport: " << statusReport
                  << std::endl;

        auto te = TEManager::getInstance().getTEById(teid_);
        if (!te) {
            LOG_ERR_EX("Failed to get te");
            return false;
        }

        auto wf = te->getWorkFlow(taskid_);
        if (!wf) {
            LOG_ERR_EX("Failed to get workflow");
            return false;
        }

        auto msg = TEHelper::newTaskStatusReport(teid_, statusCode, statusReport);
        if (!msg) {
            LOG_ERR_EX("Failed to get te");
            return false;
        }
        msg->setSender("SystemAgent");
        msg->setReceiver(wf->GetTopSender());
        msg->setTaskID(taskid_);
        msg->setClientExField(wf->GetClientExField());

        wf->ChangeStatus(taskid_, msg);
        LOG_INFO_EX("send TaskStatusReport to workflow");
        return true;
    }

    void addAssistantMessage(const std::string& userid, const std::string& clientSessionId, const std::string& content)
    {
        AddMemoryHistory(userid, ChatMessage{.sessionId = clientSessionId,
                                             .content = ChatMessageContent{
                                                 .role = "system",
                                                 .content = content,
                                             }});
    }

    std::string teid_;  // teamenvirenmentid
    std::string buffer;
    std::string taskid_;
    bool think_flag_{false};
    bool first_token_{true};
    std::chrono::steady_clock::time_point last_send_time = std::chrono::steady_clock::now();
};

#endif  // SYSAGENT_QAAGENTCALLBACK_H
