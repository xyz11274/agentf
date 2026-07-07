/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: This file declares for te_manager.cpp
 */

#include <algorithm>

#include "WorkFlow.h" // workflow
#include "taskManage/TEManager.h"
#include "securityFence/SecurityFence.h"

#include "iostream"
#include <iostream>

std::shared_ptr<TeamEnvironment> TEManager::findTEByUserId(const std::string& userId)
{
    auto it = userIdToTeamId_.find(userId);
    if (it != userIdToTeamId_.end()) {
        auto teamId = it->second;
        auto teIt = teamIdToTE_.find(teamId);
        if (teIt != teamIdToTE_.end()) {
            return teIt->second;
        }
    }
    return nullptr;
}

std::shared_ptr<TeamEnvironment> TEManager::findTEByTaskId(const std::string& taskId)
{
    // 遍历所有的TeamEnvironment
    for (const auto& pair : teamIdToTE_) {
        auto te = pair.second;

        // 检查TE是否有TaskPlanner
        if (!te->hasWorkFlow()) {
            continue;
        }

        // 获取 WorkFlow 并检查是否包含指定的Task
        auto workFlow = te->getWorkFlow(taskId);
        if (workFlow && (workFlow->FindTaskNodeByTaskID(taskId) != nullptr)) {
            return te;
        }
    }

    // 如果没有找到包含指定TaskId的TE，返回nullptr
    return nullptr;
}

void TEManager::addTE(std::shared_ptr<TeamEnvironment> te)
{
    const auto& userInfo = te->getUserInfo();
    teamIdToTE_[te->getTeamId()] = te;
    std::string userID = userInfo.userName + "_" + userInfo.deviceCategory;
    LOG_INFO_EX("userIdToTeamId_ add key:%s", userID.c_str());
    userIdToTeamId_[userID] = te->getTeamId();
}

bool TEManager::removeTE(const std::string& teamId)
{
    auto it = teamIdToTE_.find(teamId);
    if (it != teamIdToTE_.end()) {
        const auto& userInfo = it->second->getUserInfo();
        std::string userID = userInfo.userName + "_" + userInfo.deviceCategory;
        LOG_INFO_EX("userIdToTeamId_ add key:%s", userID.c_str());
        userIdToTeamId_.erase(userID);
        teamIdToTE_.erase(it);
        return true;
    }
    return false;
}

std::vector<std::shared_ptr<TeamEnvironment>> TEManager::getAllTEs() const
{
    std::vector<std::shared_ptr<TeamEnvironment>> result;
    result.reserve(teamIdToTE_.size());
    for (const auto& pair : teamIdToTE_) {
        result.push_back(pair.second);
    }
    return result;
}

// Extract user information from AccessCtrl
UserInfo TEManager::extractUserInfo(const std::shared_ptr<SYSAGENT::AccessCtrl>& data)
{
    UserInfo userInfo;

    // Extract user information based on message type
    switch (data->eventType) {
        case SYSAGENT::EventType::TASKASSIGNMENT:
            userInfo.userName = data->taskAssignment.header.userName;
            userInfo.deviceId = data->taskAssignment.header.deviceId;
            userInfo.deviceCategory = data->taskAssignment.header.deviceCategory;
            userInfo.uuid = data->taskAssignment.header.uuid;
            userInfo.clientSessionId = data->taskAssignment.header.clientSessionId;
            userInfo.acmSessionId = data->taskAssignment.header.acmSessionId;
            break;
        case SYSAGENT::EventType::TASKSTOP:
            userInfo.userName = data->taskStop.header.userName;
            userInfo.deviceId = data->taskStop.header.deviceId;
            userInfo.deviceCategory = data->taskStop.header.deviceCategory;
            userInfo.uuid = data->taskStop.header.uuid;
            userInfo.clientSessionId = data->taskStop.header.clientSessionId;
            userInfo.acmSessionId = data->taskStop.header.acmSessionId;
            break;
        case SYSAGENT::EventType::USERINPUT:
            userInfo.userName = data->userInput.header.userName;
            userInfo.userName = data->userInput.header.userName;
            userInfo.deviceId = data->userInput.header.deviceId;
            userInfo.deviceCategory = data->userInput.header.deviceCategory;
            userInfo.uuid = data->userInput.header.uuid;
            userInfo.clientSessionId = data->userInput.header.clientSessionId;
            userInfo.acmSessionId = data->userInput.header.acmSessionId;
            break;
        case SYSAGENT::EventType::DATAEXCHANGE:
            userInfo.userName = data->dataExchange.header.userName;
            userInfo.userName = data->dataExchange.header.userName;
            userInfo.deviceId = data->dataExchange.header.deviceId;
            userInfo.deviceCategory = data->dataExchange.header.deviceCategory;
            userInfo.uuid = data->dataExchange.header.uuid;
            userInfo.clientSessionId = data->dataExchange.header.clientSessionId;
            userInfo.acmSessionId = data->dataExchange.header.acmSessionId;
            break;
        case SYSAGENT::EventType::TASKSTATUSMANAGEMENT:
            userInfo.userName = data->taskStatusManagement.header.userName;
            userInfo.userName = data->taskStatusManagement.header.userName;
            userInfo.deviceId = data->taskStatusManagement.header.deviceId;
            userInfo.deviceCategory = data->taskStatusManagement.header.deviceCategory;
            userInfo.uuid = data->taskStatusManagement.header.uuid;
            userInfo.clientSessionId = data->taskStatusManagement.header.clientSessionId;
            userInfo.acmSessionId = data->taskStatusManagement.header.acmSessionId;
            break;
        case SYSAGENT::EventType::USEROUTPUT:
            userInfo.userName = data->userOutput.header.userName;
            userInfo.deviceId = data->userOutput.header.deviceId;
            userInfo.deviceCategory = data->userOutput.header.deviceCategory;
            userInfo.uuid = data->userOutput.header.uuid;
            userInfo.clientSessionId = data->userOutput.header.clientSessionId;
            userInfo.acmSessionId = data->userOutput.header.acmSessionId;
            break;
        // 添加其他类
        case SYSAGENT::EventType::LOGINOUT:
            userInfo.userName = data->loginOut.header.userName;
            userInfo.deviceId = data->loginOut.header.deviceId;
            userInfo.deviceCategory = data->loginOut.header.deviceCategory;
            userInfo.uuid = data->loginOut.header.uuid;
            userInfo.clientSessionId = data->loginOut.header.clientSessionId;
            userInfo.acmSessionId = data->loginOut.header.acmSessionId;
            break;
        case SYSAGENT::EventType::USERPORTRAITCONF:
            userInfo.userName = data->userPortrait.header.userName;
            userInfo.deviceId = data->userPortrait.header.deviceId;
            userInfo.deviceCategory = data->userPortrait.header.deviceCategory;
            userInfo.uuid = data->userPortrait.header.uuid;
            userInfo.clientSessionId = data->userPortrait.header.clientSessionId;
            userInfo.acmSessionId = data->userPortrait.header.acmSessionId;
            break;
        default:
            // Default values to avoid null values
            userInfo.userName = "Unknown User";
            LOG_INFO_EX("Could not extract user information from unknown event type: %d, using default values", static_cast<int>(data->eventType));
    }

    return userInfo;
}

// Generate team ID
std::string TEManager::generateTeamId(const UserInfo& userInfo)
{
    // Generate unique team ID based on user ID and timestamp
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    return "te_" + userInfo.userName + "_" + userInfo.deviceCategory + "_" + std::to_string(timestamp);
}

// Create TE from AccessCtrl data
std::shared_ptr<TeamEnvironment> TEManager::createTEFromData(const std::shared_ptr<SYSAGENT::AccessCtrl>& data)
{
    if (!data) {
        LOG_ERR_EX("Cannot create TeamEnvironment from null data");
        return nullptr;
    }

    LOG_INFO_EX("Creating new TeamEnvironment from data, event type: %d", static_cast<int>(data->eventType));

    // First check if there is a TeamEnvironmentID in the data
    std::string teamEnvId = extractTeamEnvId(data);
    if (!teamEnvId.empty()) {
        // Try to find existing TE by ID
        auto existingTE = getTEById(teamEnvId);
        if (existingTE) {
            LOG_INFO_EX("Found existing TeamEnvironment with ID: %s", teamEnvId.c_str());
            return existingTE;
        }
        LOG_INFO_EX("TeamEnvironmentID %s provided but no matching TE found, will create new one", teamEnvId.c_str());
    }

    // Extract user info from the data
    UserInfo userInfo = extractUserInfo(data);
    LOG_INFO_EX("Extracted user info: userName=%s", userInfo.userName.c_str());

    // Check if TE already exists for this user
    std::string userID = userInfo.userName + "_" + userInfo.deviceCategory;
    auto existingTE = findTEByUserId(userID);
    if (existingTE) {
        existingTE->updateUserInfo(userInfo);
        LOG_INFO_EX("TeamEnvironment already exists for user: %s, returning existing instance", userInfo.userName.c_str());
        return existingTE;
    }

    // Use provided TeamEnvironmentID or generate a new one
    std::string teamId = teamEnvId.empty() ? generateTeamId(userInfo) : teamEnvId;
    LOG_INFO_EX("Using team ID: %s", teamId.c_str());

    // Create new TeamEnvironment
    auto newTE = TeamEnvironment::createTE(teamId, userInfo);
    if (!newTE) {
        LOG_ERR_EX("Failed to create TeamEnvironment instance");
        return nullptr;
    }

    // Add to manager
    addTE(newTE);
    LOG_INFO_EX("Successfully created and added new TeamEnvironment with ID: %s", teamId.c_str());

    return newTE;
}

// 从AccessCtrl中提取TeamEnvironmentID
std::string TEManager::extractTeamEnvId(const std::shared_ptr<SYSAGENT::AccessCtrl>& data)
{
    if (!data) {
        return "";
    }

    // 根据不同的事件类型获取teamEnvironmentID
    switch (data->eventType) {
        case SYSAGENT::EventType::TASKASSIGNMENT:
            return data->taskAssignment.header.teamEnvironmentID;
        case SYSAGENT::EventType::USERINPUT:
            return data->userInput.header.teamEnvironmentID;
        case SYSAGENT::EventType::DATAEXCHANGE:
            return data->dataExchange.header.teamEnvironmentID;
        case SYSAGENT::EventType::TASKSTATUSMANAGEMENT:
            return data->taskStatusManagement.header.teamEnvironmentID;
        case SYSAGENT::EventType::USEROUTPUT:
            return data->userOutput.header.teamEnvironmentID;
        case SYSAGENT::EventType::LOGINOUT:
            return data->loginOut.header.teamEnvironmentID;
        case SYSAGENT::EventType::TASKSTOP:
            return data->taskStop.header.teamEnvironmentID;
        case SYSAGENT::EventType::USERPORTRAITCONF:
            return data->userPortrait.header.teamEnvironmentID;
        default:
            LOG_INFO_EX("Unknown event type: %d when extracting TeamEnvironmentID", static_cast<int>(data->eventType));
            return "";
    }
}

// 根据teamEnvironmentID查找TE
std::shared_ptr<TeamEnvironment> TEManager::getTEById(const std::string& teamEnvId)
{
    if (teamEnvId.empty()) {
        LOG_INFO_EX("Empty teamEnvironmentID provided to getTEById");
        return nullptr;
    }

    LOG_INFO_EX("Looking for TeamEnvironment with ID: %s", teamEnvId.c_str());

    auto it = teamIdToTE_.find(teamEnvId);
    if (it != teamIdToTE_.end()) {
        LOG_INFO_EX("Found TeamEnvironment with ID: %s", teamEnvId.c_str());
        return it->second;
    }

    LOG_INFO_EX("TeamEnvironment with ID: %s not found", teamEnvId.c_str());
    return nullptr;
}