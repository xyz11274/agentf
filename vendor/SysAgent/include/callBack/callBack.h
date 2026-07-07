/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: This file declares for callback.h
 */

#ifndef SYSAGENT_CALLBACK_H
#define SYSAGENT_CALLBACK_H

#pragma once

#include <string>
#include <memory>
#include <map>
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "callBack/TaskRouter.h"

constexpr char HEALTH_CHECK_URL[] = "/healthcheck";

constexpr char SECURITY_FENCE_CHECK_URL[] = "/securityfence";

constexpr char USER_REQUEST_URL[] = "/coremind/systemagent/v1/callback";

constexpr char MEM_RECALL_URL[] = "/memAgent/v1/user/memories/recall";

constexpr char MEM_QUERY_URL[] = "/memAgent/v1/user/memories/query";

constexpr char PERSONA_QUERY_URL[] = "/memAgent/v1/user/memories/persona/query";

constexpr char DEVICE_INSTANCE_NUM_QUERY[] = "/device/instanceNum";

constexpr char AGENT_INSTALL_URL[] = "/aisf/sysagent/v1/agentInstall";

constexpr char AGENT_UPDATE_URL[] = "/aisf/sysagent/v1/agentUpdate";

constexpr char AGENT_UNINSTALL_URL[] = "/aisf/sysagent/v1/agentUninstall";

constexpr char SCHEDULE_TASK_ABILITY_URL[] = "/sysagent/v1/scheduleTaskAbility";

// SysAgent 下发的健康任务入口
constexpr char HEALTH_AGENT_TASK_URL[] = "/aisf/healthagent/v1/tasks";
// 前置声明，避免循环引用
class Task;

// Agent 消息请求处理入口（REST 和 UDS 共用）
CMFrm::COM::HttpStatus HandleAgentMsgRequest(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl);

class SysCallBack {
public:
    virtual ~SysCallBack() = default;

    // 解析 sender 字段的静态方法
    static std::string ParseSender(const std::shared_ptr<SYSAGENT::AccessCtrl>& access);

    static void SysAgentMsgCallBack(const std::shared_ptr<SYSAGENT::AccessCtrl>& access,
        const std::string& dialogueId = "", const std::string& sessionId = "");

    // 解析 receiver 字段的静态方法
    static std::string ParseReceiver(const std::shared_ptr<SYSAGENT::AccessCtrl>& access);

    static bool RegisterRouter();

    static std::shared_ptr<SYSAGENT::TaskRouter> GetRouterMgr();

private:
    static std::shared_ptr<SYSAGENT::TaskRouter> routerMgr;

    static CMFrm::COM::ServerRespHandleMode HandleHealthCheck(
            const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

    static CMFrm::COM::ServerRespHandleMode HandleUserRequest(
            const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

    static CMFrm::COM::ServerRespHandleMode HandleMemoryRecall(
            const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

    static CMFrm::COM::ServerRespHandleMode HandleMemoryQuery(
            const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

    static CMFrm::COM::ServerRespHandleMode HandlePersonaQuery(
            const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

    static CMFrm::COM::ServerRespHandleMode HandleAgentInstallRequest(
            const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

    static CMFrm::COM::ServerRespHandleMode HandleAgentUpdateRequest(
            const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

    static CMFrm::COM::ServerRespHandleMode HandleAgentUninstallRequest(
            const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

    static CMFrm::COM::ServerRespHandleMode HandleSecurityFenceRequest(
            const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

    static CMFrm::COM::ServerRespHandleMode HandleQueryInstanceNumRequest(
            const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

    static CMFrm::COM::ServerRespHandleMode HandleScheduleTaskAbility(
            const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

    static Json::Value BuildScheduleTaskAbilityResponse(const std::string& userId);
};

#endif //SYSAGENT_CALLBACK_H