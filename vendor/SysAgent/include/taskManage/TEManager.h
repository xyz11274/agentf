/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: This file declares for te_manager.h
 */

#ifndef SYSAGENT_TE_MANAGER_H
#define SYSAGENT_TE_MANAGER_H

#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "taskManage/TeamEnvironment.h"
#include <memory>
#include <vector>

class TEManager {
public:
    // 单例模式获取实例
    static TEManager& getInstance()
    {
        static TEManager instance;
        return instance;
    }
    // 根据AccessCtrl创建TE
    std::shared_ptr<TeamEnvironment> createTEFromData(const std::shared_ptr<SYSAGENT::AccessCtrl>& data);

    // 根据userId查找TE
    std::shared_ptr<TeamEnvironment> findTEByUserId(const std::string& userId);

    // 根据taskId查找TE
    std::shared_ptr<TeamEnvironment> findTEByTaskId(const std::string& taskId);
    
    // 根据teamEnvironmentID查找TE
    std::shared_ptr<TeamEnvironment> getTEById(const std::string& teamEnvId);

    // 添加新的TE
    void addTE(std::shared_ptr<TeamEnvironment> te);

    // 删除TE
    bool removeTE(const std::string& teamId);

    // 获取所有TE
    std::vector<std::shared_ptr<TeamEnvironment>> getAllTEs() const;

    std::unordered_map<std::string, std::shared_ptr<TeamEnvironment>> teamIdToTE_;

    // 从AccessCtrl中提取TeamEnvironmentID
    std::string extractTeamEnvId(const std::shared_ptr<SYSAGENT::AccessCtrl>& data);

private:
    TEManager() = default; // 私有构造函数
    ~TEManager() = default;

    TEManager(const TEManager&) = delete;
    TEManager& operator=(const TEManager&) = delete;

    // 从AccessCtrl中提取用户信息
    UserInfo extractUserInfo(const std::shared_ptr<SYSAGENT::AccessCtrl>& data);

    // 生成唯一的团队ID
    std::string generateTeamId(const UserInfo& userInfo);

    std::unordered_map<std::string, std::string> userIdToTeamId_;
};

#endif // SYSAGENT_TE_MANAGER_H
