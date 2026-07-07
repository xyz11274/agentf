/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: This file declares for te_helper.h
 */

#ifndef SYSAGENT_TE_HELPER_H
#define SYSAGENT_TE_HELPER_H

#pragma once
#ifdef Expand
#undef Expand
#endif
#include <memory>
#include "WorkFlow.h"
#include "taskManage/TEManager.h"
#include "taskManage/TeamEnvironment.h"

// 团队环境辅助工具类，提供与TeamEnvironment相关的辅助功能
class TEHelper {
public:
    // 为团队环境创建任务规划器
    static std::shared_ptr<WorkFlow> CreateWorkFlow(std::shared_ptr<TeamEnvironment> te, std::string& userName,
                                                    std::string workflowID = "");

    // 处理来自下游Agent的请求，更新任务状态
    static bool ProcessRequest(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl);

    // 根据AccessCtrl获取或创建TeamEnvironment
    static std::shared_ptr<TeamEnvironment> GetOrCreateTE(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl, PerSession* session = nullptr);

    static void CleanTE(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl, PerSession* session);

    static void RunGraph(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl);

    // 从AccessCtrl中提取TaskID
    static std::string extractTaskId(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl);

    static std::string extractTEidFromMessage(const std::shared_ptr<SYSAGENT::AccessCtrl>& data);

    static void saveTeamEnvID(std::shared_ptr<SYSAGENT::AccessCtrl> accCtr, std::string envId);

    // 从AccessCtrl中提取用户信息
    static UserInfo extractUserInfo(const std::shared_ptr<SYSAGENT::AccessCtrl>& data);

    static std::shared_ptr<SYSAGENT::AccessCtrl> newUserOutput(std::shared_ptr<TeamEnvironment> te, std::string output, std::string status, int flag);
    static std::shared_ptr<SYSAGENT::AccessCtrl> newUserOutput(std::shared_ptr<TeamEnvironment> te, std::string output,  std::string outputObject, std::string status, int flag);

    static std::shared_ptr<SYSAGENT::AccessCtrl> newUserOutput(std::string teid, std::string output, std::string status, int flag);

    static std::shared_ptr<SYSAGENT::AccessCtrl> newTaskStatusReport(std::string teid, int code, std::string report);

    static std::shared_ptr<SYSAGENT::AccessCtrl> newTaskStatusReport(std::shared_ptr<TeamEnvironment> te, int code, std::string report);

    static std::shared_ptr<SYSAGENT::AccessCtrl> newTaskStop(std::shared_ptr<TeamEnvironment> te);

    static std::shared_ptr<SYSAGENT::AccessCtrl> newUserTaskStop(const std::shared_ptr<TeamEnvironment>& te);

    static void sendUserTaskCompleted(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl, const std::string&
    workflowID = "");

    static void sendUserOutputMsg(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl,
                           string content, string status, int flag);

    static void sendUserOutputMsg(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl,
                                  string userOutputText, string userOutputObject, string status, int flag);

    static void sendNewTaskStatusReport(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl);

    static std::shared_ptr<WorkFlow> GetWorkFlow(const std::shared_ptr<AccessCtrl>& accessCtrl);

private:
    // 私有构造函数，防止实例化
    TEHelper() = default;

    // 禁止拷贝和赋值
    TEHelper(const TEHelper&) = delete;
    TEHelper& operator=(const TEHelper&) = delete;
};

#endif // SYSAGENT_TE_HELPER_H