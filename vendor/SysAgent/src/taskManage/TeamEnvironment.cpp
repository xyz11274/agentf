/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: This file declares for team_environment.cpp
 */

#include <iostream>

#include "framework_common/utils/env_util.h"
#include "framework_common/utils/basetype_util.h"

#include "TeamEnvironment.h"
#include "taskManage/TeamEnvironment.h"
#include "mm_customedreact_llm_component.h"
#include "model/ReactLLMTask.h"
#include "TeHelper.h"
#include "queue/QueueManager.h"
#include "config/Config.h"

#define ENV_TASKFLOW_TIMEOUT_SEC "ENV_TASKFLOW_TIMEOUT_SEC"
#define STOP_GUI_MSG "{\"code\":300001,\"data\":{\"type\":\"STOPGUI\"}}"
#define STOP_TASK_MSG "当前系统繁忙, 您的请求执行超时"

// Rag结构体方法实现
void Rag::add_query(const std::string& new_query)
{
    if (query.size() >= MAX_QUERIES) {
        query.pop_front();
    }
    query.push_back(new_query);
}

// TeamEnvironment构造函数实现
TeamEnvironment::TeamEnvironment(const std::string& teamId, const UserInfo& userInfo) : teamId_(teamId), userInfo_(userInfo)
{
    // 注意：构造函数中不再自动创建TaskPlanner
    auto workFlowTimeoutStr = ConfigManager::getInstance().getEnvInfo(ENV_TASKFLOW_TIMEOUT_SEC);
    if (!workFlowTimeoutStr.empty()) {
        auto envSec = CMFrm::Utils::ParseInt(workFlowTimeoutStr);
        workFlowTimeoutSecond = envSec > 0 ? static_cast<uint32_t>(envSec) : WORKFLOW_TIMEOUT_SECONDS;
    }
}

// 静态工厂方法实现
std::shared_ptr<TeamEnvironment> TeamEnvironment::createTE(const std::string& teamId, const UserInfo& userInfo)
{

    auto teamEnvironment = std::make_shared<TeamEnvironment>(teamId, userInfo);
    return teamEnvironment;
}

// RAG查询管理方法实现
void TeamEnvironment::addQuery(const std::string& query)
{
    rag_.add_query(query);
}

const std::deque<std::string>& TeamEnvironment::getQueries() const
{
    return rag_.query;
}

// 额外信息管理
void TeamEnvironment::setExtraInfo(const std::string& key, const std::string& value)
{
    extraInfo_[key] = value;
}

std::string TeamEnvironment::getExtraInfo(const std::string& key) const
{
    auto it = extraInfo_.find(key);
    if (it != extraInfo_.end()) {
        return it->second;
    }
    return "";
}

// TaskPlanner管理方法实现
void TeamEnvironment::addWorkFlow(std::shared_ptr<WorkFlow> workFlow)
{
    std::unique_lock<std::shared_mutex> lk(this->workFlowMutex_);
    if (!workFlow) {
        LOG_STDERR("Warning: Attempting to add a null WorkFlow to TeamEnvironment ");
        return;
    }

    if (workFlow_) {
        LOG_STDOUT("Replacing existing workFlow in TeamEnvironment ");
        // 这里可以添加清理旧TaskPlanner的代码，如果需要的话
    }

    workFlow_ = workFlow;
}

bool TeamEnvironment::hasWorkFlow()
{
    std::shared_lock<std::shared_mutex> lk(this->workFlowMutex_);
    return workFlow_ != nullptr;
}

std::shared_ptr<WorkFlow> TeamEnvironment::getWorkFlow(const std::string& taskID)
{
    std::shared_lock<std::shared_mutex> lk(this->workFlowMutex_);
    if (workFlow_ == nullptr) {
        return nullptr;
    }
    size_t index = taskID.find(workFlow_->GetWorkflowID());
    if (index == std::string::npos) {
        return nullptr;
    }
    return workFlow_;
}

std::shared_ptr<WorkFlow> TeamEnvironment::getWorkFlow()
{
    std::shared_lock<std::shared_mutex> lk(this->workFlowMutex_);
    return workFlow_;
}

// Getter方法实现
const std::string& TeamEnvironment::getTeamId() const
{
    return teamId_;
}

const UserInfo& TeamEnvironment::getUserInfo() const
{
    return userInfo_;
}

// update userinfo
void TeamEnvironment::updateUserSession(const std::string& clientSessionId, const std::string& acmSessionId)
{
    userInfo_.acmSessionId = acmSessionId;
    userInfo_.clientSessionId = clientSessionId;
}

// update userinfo
void TeamEnvironment::updateUserInfo(const UserInfo& userInfo)
{
    userInfo_ = userInfo;
}

/**
 * 获取当前 TeamEnvironment 关联的会话指针
 * @return 会话指针，如果没有关联的会话则返回 nullptr
 */
PerSession* TeamEnvironment::getSession()
{
    // 简单地返回成员变量 session
    // 注意：这里假设 TeamEnvironment 类中有一个 PerSession* session 成员变量
    if (!session) {
        LOG_WARN("TeamEnvironment %s has no associated session", getTeamId().c_str());
    }
    return session;
}

void TeamEnvironment::deleteWorkFlow()
{
    std::unique_lock<std::shared_mutex> lk(this->workFlowMutex_);
    workFlow_ = nullptr;
}

void TeamEnvironment::compareAndDeleteWorkFlow(const std::shared_ptr<WorkFlow>& workFlow)
{
    std::unique_lock<std::shared_mutex> lk(this->workFlowMutex_);
    if (workFlow_ != workFlow) {
        return;
    }
    workFlow_ = nullptr;
}

void TeamEnvironment::setIntentState(const IntentState& state)
{
    intentstate_ = state;
}

const IntentState& TeamEnvironment::getIntentState()
{
    return intentstate_;
}

void TeamEnvironment::setIntentTask(const std::string& taskid)
{
    taskId_ = taskid;
}

const std::string& TeamEnvironment::getIntentTask()
{
    return taskId_;
}

void TeamEnvironment::setQATask(const std::string& taskid)
{
    QATaskId_ = taskid;
}

const std::string& TeamEnvironment::getQATask()
{
    return QATaskId_;
}

void TeamEnvironment::setLModelTaskId(const std::string& taskId)
{
    LModelTaskId_ = taskId;
}

const std::string& TeamEnvironment::getLModelTaskId()
{
    return LModelTaskId_;
}

void TeamEnvironment::setQALastIntent(const std::string& lastIntent)
{
    QALastIntent_ = lastIntent;
}

const std::string& TeamEnvironment::getQALastIntent()
{
    return QALastIntent_;
}

void TeamEnvironment::setQACallback(std::shared_ptr<mmsdk::MMLLMAsyncCallBack> streamCallback)
{
    streamCallback_ = streamCallback;
}
const std::shared_ptr<mmsdk::MMLLMAsyncCallBack>& TeamEnvironment::getQACallback()
{
    return streamCallback_;
}

void TeamEnvironment::setLModelTask(std::shared_ptr<ReactLLMTask>& readLLMtask)
{
    lModelTask_ = readLLMtask;
}

const std::shared_ptr<ReactLLMTask>& TeamEnvironment::getLModelTask()
{
    return lModelTask_;
}

void TeamEnvironment::setToolTaskId(const std::string& taskId)
{
    ToolTaskId_ = taskId;
}

const string& TeamEnvironment::getToolTaskId()
{
    return ToolTaskId_;
}

void TeamEnvironment::setToolTask(std::shared_ptr<ReactLLMTask>& readLLMtask)
{
    toolTask_ = readLLMtask;
}

const std::shared_ptr<ReactLLMTask>& TeamEnvironment::getToolTask()
{
    return toolTask_;
}

void TeamEnvironment::stopTimeoutWorkFlow()
{
    // 主动终止当前超时的workflow
    std::unique_lock<std::shared_mutex> lk(workFlowMutex_);
    if (workFlow_ == nullptr) {
        return;
    }
    if (!workFlow_->CheckWorkFlowTimeOut(workFlowTimeoutSecond)) {
        return;
    }
    LOG_INFO_EX("Clean Timeout WorkFlow");
    // 如果是GUI任务执行中，首先发一个GUI STOP
    if (workFlow_->GetNowSender() == "GUIAgent") {
        addGUIStopMsg();
    }
    // 模拟用户发送结束报文
    auto stopMsg = TEHelper::newTaskStop(shared_from_this());
    stopMsg->setSender(workFlow_->GetTopSender());
    stopMsg->setReceiver("SystemAgent");
    workFlow_->StopTask(stopMsg);
    // 向用户发送提示消息,提示用户任务已经结束
    addUserStopAlarmMsg();
    // 向用户发送结束信息，提示任务已经结束
    addUserStopMsg();
    workFlow_->SetWorkflowStatusFinished();
    workFlow_ = nullptr;
}

void TeamEnvironment::addGUIStopMsg()
{
    auto stopGUIMsg = TEHelper::newUserOutput(shared_from_this(), "", STOP_GUI_MSG, "Completed", 1);
    CHECK_NULL_LOG_ERROR_RETURN_VOID(stopGUIMsg, "stopGUIMsg is null");
    stopGUIMsg->setSender("GUIAgent");
    stopGUIMsg->setReceiver(workFlow_->GetTopSender());
    stopGUIMsg->setTaskID(workFlow_->GetNowTaskId());
    stopGUIMsg->setClientExField(workFlow_->GetClientExField());
    QueueManager::getInstance().postWriteQueue(stopGUIMsg, session);
}

void TeamEnvironment::addUserStopAlarmMsg()
{
    auto notifyUserMsg = TEHelper::newUserOutput(shared_from_this(), STOP_TASK_MSG, "Completed", 0);
    CHECK_NULL_LOG_ERROR_RETURN_VOID(notifyUserMsg, "notifyUserMsg is null");
    notifyUserMsg->setSender("SystemAgent");
    notifyUserMsg->setReceiver(workFlow_->GetTopSender());
    notifyUserMsg->setTaskID(workFlow_->GetNowTaskId());
    notifyUserMsg->setClientExField(workFlow_->GetClientExField());
    QueueManager::getInstance().postWriteQueue(notifyUserMsg, session);
}

void TeamEnvironment::addUserStopMsg()
{
    auto notifyUserMsg = TEHelper::newUserTaskStop(shared_from_this());
    CHECK_NULL_LOG_ERROR_RETURN_VOID(notifyUserMsg, "notifyUserMsg is null");
    notifyUserMsg->setSender("SystemAgent");
    notifyUserMsg->setReceiver(workFlow_->GetTopSender());
    notifyUserMsg->setTaskID(workFlow_->GetNowTaskId());
    notifyUserMsg->setClientExField(workFlow_->GetClientExField());
    QueueManager::getInstance().postWriteQueue(notifyUserMsg, session);
}
