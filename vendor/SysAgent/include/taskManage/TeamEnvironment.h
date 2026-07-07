/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: This file declares for team_environment.h
 */

#ifndef SYSAGENT_TEAM_ENVIRONMENT_H
#define SYSAGENT_TEAM_ENVIRONMENT_H

#pragma once

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>

#include "mm_customedreact_llm_component.h"

#include "WorkFlow.h"
#include "queue/QueueEntity.h"
#include "model/ReactLLMTask.h"

#define WORKFLOW_TIMEOUT_SECONDS 60

enum class IntentState {
    Intent,
    Clarify,
};

// 用户信息结构体
struct UserInfo {
    std::string userName;
    std::string deviceId;
    std::string deviceCategory;
    std::string uuid;
    std::string clientSessionId;
    std::string acmSessionId;
};

// RAG查询历史管理结构体
struct Rag {
    std::deque<std::string> query;
    static const size_t MAX_QUERIES = 10;

    void add_query(const std::string& new_query);
};

class TeamEnvironment : public std::enable_shared_from_this<TeamEnvironment> {
public:
    // 构造函数
    TeamEnvironment(const std::string& teamId, const UserInfo& userInfo);

    // 创建Team Environment的静态工厂方法
    static std::shared_ptr<TeamEnvironment> createTE(const std::string& teamId, const UserInfo& userInfo);

    // RAG查询管理
    void addQuery(const std::string& query);
    const std::deque<std::string>& getQueries() const;

    // 额外信息管理
    void setExtraInfo(const std::string& key, const std::string& value);
    std::string getExtraInfo(const std::string& key) const;

    // WorkFlow管理
    void addWorkFlow(std::shared_ptr<WorkFlow> workFlow);
    bool hasWorkFlow();
    std::shared_ptr<WorkFlow> getWorkFlow(const std::string& taskID);
    std::shared_ptr<WorkFlow> getWorkFlow();
    void stopTimeoutWorkFlow();

    // Getter方法
    const std::string& getTeamId() const;
    const UserInfo& getUserInfo() const;
    void updateUserSession(const std::string& clientSessionId, const std::string& acmSessionId);
    void updateUserInfo(const UserInfo& userInfo);

    std::string sender;
    std::string clientExField;

    PerSession* session{nullptr};
    PerSession* getSession();
    void deleteWorkFlow();
    void compareAndDeleteWorkFlow(const std::shared_ptr<WorkFlow>& workFlow);

    void setIntentState(const IntentState& state);
    const IntentState& getIntentState();

    void setIntentTask(const std::string& taskid);
    const std::string& getIntentTask();

    void setQATask(const std::string& taskid);
    const std::string& getQATask();

    void setQALastIntent(const std::string& lastIntent);
    const std::string& getQALastIntent();

    void setLModelTaskId(const std::string& taskId);
    const std::string& getLModelTaskId();

    void setQACallback(std::shared_ptr<mmsdk::MMLLMAsyncCallBack> streamCallback);
    const std::shared_ptr<mmsdk::MMLLMAsyncCallBack>& getQACallback();

    void setToolTaskId(const std::string& taskId);
    const string& getToolTaskId();

    void setLModelTask(std::shared_ptr<ReactLLMTask>& readLLMtask);
    const std::shared_ptr<ReactLLMTask>& getLModelTask();

    void setToolTask(std::shared_ptr<ReactLLMTask>& readLLMtask);
    const std::shared_ptr<ReactLLMTask>& getToolTask();

    bool thinkFlag{false};

    int rearFenceSeqNum{1};    // 从1开始
    unsigned int fenceBusinessId{0};
    int frontFenceLevel{0}; // 默认为无风险放通

private:
    std::string taskId_{"-1"};   // 意图感知使用的taskid
    std::string QATaskId_{"-1"}; // QA问答使用的taskid
    std::string LModelTaskId_{"-1"}; // 慢意图使用的taskid
    std::string ToolTaskId_{"-1"}; // ToolTask使用的taskid
    std::string teamId_;
    IntentState intentstate_{IntentState::Intent};
    std::string QALastIntent_{"-1"}; // 初始为空，第一次进来时，创建新的task
    std::shared_ptr<mmsdk::MMLLMAsyncCallBack> streamCallback_{nullptr};
    std::shared_ptr<ReactLLMTask> lModelTask_{nullptr};
    std::shared_ptr<ReactLLMTask> toolTask_{nullptr};
    UserInfo userInfo_;
    Rag rag_;

    std::shared_mutex workFlowMutex_;
    std::shared_ptr<WorkFlow> workFlow_{nullptr};
    uint32_t workFlowTimeoutSecond{WORKFLOW_TIMEOUT_SECONDS};

    // 额外信息存储
    std::unordered_map<std::string, std::string> extraInfo_;

private:
    /**
     * 添加GUI停止消息
     */
    void addGUIStopMsg();

    /**
     * 添加用户任务终止提示信息
     */
    void addUserStopAlarmMsg();

    /**
     * 添加用户停止信息
     */
    void addUserStopMsg();

};

#endif // SYSAGENT_TEAM_ENVIRONMENT_H
