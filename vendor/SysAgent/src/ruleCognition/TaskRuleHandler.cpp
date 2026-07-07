//
// Created by z00913804 on 25-7-28.
//
#include "ruleCognition/TaskRuleHandler.h"
#include "taskManage/TEManager.h"
#include "TeHelper.h"

shared_ptr<Rule> TaskRuleHandler::doChain(const std::shared_ptr<AccessCtrl>& accessCtrl)
{
    LOG_INFO_EX("TaskRuleHandler enter");
    if (accessCtrl == nullptr) {
        LOG_ERR_EX("[TaskRuleHandler::doChain] accessCtrl is nullptr");
        return nullptr;
    }
    std::string taskID = accessCtrl->header.taskID;
    // 任务创建, 这里返回空，后续指定意图和定制意图会进行处理, 统一意图进行兜底
    if (taskID.empty() && (accessCtrl->eventType == EventType::TASKSTOP ||
    (accessCtrl->eventType == EventType::USERINPUT && accessCtrl->userInput.data.userInputText.empty()))) {
        UserInfo userInfo = TEHelper::extractUserInfo(accessCtrl);
        std::string userId = userInfo.userName + "_" + accessCtrl->extractDeviceCategory();
        auto te = TEManager::getInstance().findTEByUserId(userId);
        LOG_INFO("Enter TaskStop te get workflow process %s", userId.c_str());
        std::string workflowID;
        if (te) {
            LOG_INFO("Enter TaskStop te get workflow process");
            if (te->getWorkFlow()) {
                workflowID = te->getWorkFlow()->GetWorkflowID();
                te->getWorkFlow()->StopTask(accessCtrl);
                te->deleteWorkFlow();
            }
        } else {
            LOG_INFO("Enter TaskStop not found");
        }
        TEHelper::sendUserOutputMsg(accessCtrl, "", UserOutput_Completed, 0);
        TEHelper::sendUserTaskCompleted(accessCtrl);
        shared_ptr<Rule> rule = make_shared<Rule>();
        rule->ruleName = RuleName::TASK_RULE_HANDLER;
        rule->nextNode = "EndNode";
        return rule;
    } else if (taskID.empty() && accessCtrl->eventType != EventType::USERINPUT) {
        return nullptr;
    }
    // tasKId 存在 && workflow 已经被删除了
    auto teamEnv = TEManager::getInstance().getTEById(accessCtrl->header.teamEnvironmentID);
    if (teamEnv) {
        auto workFlow = teamEnv->getWorkFlow();
        if (workFlow) {
            shared_ptr<Rule> rule = make_shared<Rule>();
            rule->ruleName = RuleName::TASK_RULE_HANDLER;
            rule->nextNode = "ExecuteNode"; // 澄清和交互，走执行节点
            return rule;
        }
    }
    accessCtrl->eventType = EventType::TASKASSIGNMENT;
    accessCtrl->header.taskID = "";
    accessCtrl->header.messageType = "UserTaskManagement";
    accessCtrl->taskAssignment.header = accessCtrl->header;
    accessCtrl->taskAssignment.data.messageType = "TaskAssignment";
    accessCtrl->taskAssignment.data.userInputObject = accessCtrl->userInput.data.userInputObject;
    accessCtrl->taskAssignment.data.userIntent = accessCtrl->userInput.data.userIntent;
    accessCtrl->taskAssignment.data.userInputText = accessCtrl->userInput.data.userInputText;
    return nullptr;
}
