/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: This file implements for te_helper.cpp
 */

#include "taskManage/TeHelper.h"

#include "aisf_platform/common_define.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "aisf_platform/utils/utils.h"
#include "graphManager/GraphData.h"
#include "graphManager/GraphManager.h"
#include "graphManager/GraphUtils.h"
#include "mm_customedreact_llm_component.h"
#include <queue/QueueManager.h>
#include <utility>
#include <future>
#include "model/ReactLLMTask.h"

std::shared_ptr<WorkFlow> TEHelper::CreateWorkFlow(std::shared_ptr<TeamEnvironment> te, std::string& userName,
                                                   std::string workflowID)
{
    if (!te) {
        return nullptr;
    }

    // 这里创建 workFlow还是由 TEHelper 来触发
    // 一个TE是一个用户的信息，在用户端目前只有一个窗口，因此和workflow是一一对应的
    shared_ptr<WorkFlow> workFlow = std::make_shared<WorkFlow>(workflowID);
    workFlow->SetUserName(userName);
    te->addWorkFlow(workFlow);
    return workFlow;
}

std::string TEHelper::extractTaskId(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl)
{
    if (!accessCtrl) {
        return "";
    }

    switch (accessCtrl->eventType) {
        case SYSAGENT::EventType::TASKASSIGNMENT:
            return accessCtrl->taskAssignment.header.taskID;
        case SYSAGENT::EventType::TASKSTOP:
            return accessCtrl->taskStop.header.taskID;
        case SYSAGENT::EventType::USERINPUT:
            return accessCtrl->userInput.header.taskID;
        case SYSAGENT::EventType::USEROUTPUT:
            return accessCtrl->userOutput.header.taskID;
        case SYSAGENT::EventType::DATAEXCHANGE:
            return accessCtrl->dataExchange.header.taskID;
        case SYSAGENT::EventType::TASKSTATUSMANAGEMENT:
            return accessCtrl->taskStatusManagement.header.taskID;
        case SYSAGENT::EventType::LOGINOUT:
            return accessCtrl->loginOut.header.taskID;
        case SYSAGENT::EventType::USERPORTRAITCONF:
            return accessCtrl->userPortrait.header.taskID;
        default:
            return "";
    }
}

std::shared_ptr<TeamEnvironment> TEHelper::GetOrCreateTE(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl,
                                                         PerSession* session)
{
    if (!accessCtrl) {
        LOG_ERR_EX("[GetOrCreateTE] AccessCtrl pointer is null");
        return nullptr;
    }

    std::shared_ptr<TeamEnvironment> teamEnv = nullptr;
    TEManager& teManager = TEManager::getInstance();

    // Step 1: 根据TE id获取TE
    std::string teamEnvId = teManager.extractTeamEnvId(accessCtrl);
    if (!teamEnvId.empty()) {
        LOG_INFO_EX("[GetOrCreateTE] Found TeamEnvironmentID: %s, attempting to retrieve TE", teamEnvId.c_str());
        teamEnv = teManager.getTEById(teamEnvId);
        if (teamEnv) {
            LOG_INFO_EX("[GetOrCreateTE] Successfully retrieved TeamEnvironment by ID: %s", teamEnvId.c_str());
        }
    }

    // Step 2: 根据Task id获取TE
    if (!teamEnv) {
        std::string taskId = extractTaskId(accessCtrl);
        if (!taskId.empty()) {
            LOG_INFO_EX("[GetOrCreateTE] Attempting to find TeamEnvironment by task ID: %s", taskId.c_str());
            teamEnv = teManager.findTEByTaskId(taskId);
            if (teamEnv) {
                LOG_INFO_EX("[GetOrCreateTE] Successfully retrieved TeamEnvironment by task ID: %s", taskId.c_str());
                saveTeamEnvID(accessCtrl, teamEnv->getTeamId());
                LOG_INFO_EX("[GetOrCreateTE] insert teId ok : %s", accessCtrl->header.teamEnvironmentID.c_str());
            }
        }
    }

    // Step 3: 如果都没有，就创建TE
    if (!teamEnv) {
        LOG_INFO_EX("[GetOrCreateTE] No existing TeamEnvironment found, creating new one");
        teamEnv = teManager.createTEFromData(accessCtrl);
        saveTeamEnvID(accessCtrl, teamEnv->getTeamId());
    }

    // 设置session（如果提供了）
    if (teamEnv && session) {
        teamEnv->session = session;
    }

    return teamEnv;
}

void TEHelper::CleanTE(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl, PerSession* session)
{
    std::shared_ptr<TeamEnvironment> teamEnv;
    TEManager& teManager = TEManager::getInstance();
    std::string teamEnvId = teManager.extractTeamEnvId(accessCtrl);
    if (!teamEnvId.empty()) {
        LOG_INFO_EX("Found TeamEnvironmentID: %s, attempting to retrieve TE", teamEnvId.c_str());
        teamEnv = teManager.getTEById(teamEnvId);
        if (teamEnv) {
            LOG_INFO_EX("Successfully retrieved TeamEnvironment by ID: %s", teamEnvId.c_str());
        }
    }

    // Step 2: 根据Task id获取TE
    // 不再根据taskid获取TE

    // Step 2: 根据用户名获取：用户更换设备登录，TE保持不变，设备类型，session会变
    if (!teamEnv) {
        UserInfo userInfo = extractUserInfo(accessCtrl);
        std::string userID = userInfo.userName + "_" + userInfo.deviceCategory;
        teamEnv = teManager.findTEByUserId(userID);
        LOG_INFO_EX("Successfully retrieved TeamEnvironment by username: %s", userInfo.userName.c_str());
    }

    if (!teamEnv) {
        LOG_INFO_EX("No existing TeamEnvironment found, exit");
        return;
    }

    auto wf = teamEnv->getWorkFlow();
    if (wf) {
        LOG_INFO_EX("Clean TeamEnvironment WorkFlow");
        // 发送结束报文，不需要设置taskid
        auto stopmsg = TEHelper::newTaskStop(teamEnv);
        stopmsg->setSender("APP");
        stopmsg->setReceiver("SystemAgent");
        wf->StopTask(stopmsg);
    }
    LOG_INFO_EX("Clean TeamEnvironment");
    // 异步销毁task
    auto localReactLLMomponent = reactLLMomponent;
    std::async(std::launch::async, [localReactLLMomponent, teamEnv]() {
        return reactLLMomponent->DestroyTask(teamEnv->getLModelTaskId(),
                                             [](uint32_t errorCode, std::string taskId) -> uint32_t {
                                                 LOG_INFO_EX("destroy llm task: %s, %d", taskId.c_str(), errorCode);
                                                 return 0;
                                             });
    });
    teManager.removeTE(teamEnvId);
}

void TEHelper::RunGraph(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl)
{
    auto agentgraph = GraphManager::GetInstance().GetAgentGraph(AGENT_GRAPH_INTENT);
    if (!agentgraph) {
        LOG_ERR_EX("get agentgraph failed: %s", extractTEidFromMessage(accessCtrl).c_str());
        return;
    }
    auto inputMsg = GraphUtils::NewMessage(agentgraph);
    GraphUtils::AddMessageData(inputMsg, GRAPH_DATA_ACCESSCTRL, accessCtrl);

    auto config = mmagentflow::MMRequestConfig{
            .timeout = std::chrono::seconds(120),
            .errRunFunc = [=](uint64_t requestId, uint32_t errorCode) -> void {
                std::cout << "requestId: " << requestId << ", errorCode: " << errorCode << std::endl;
            },
            .canTrace = true,
            .traceFunc = [=](uint64_t requestId, std::string& msg) -> void { std::cout << requestId << msg << std::endl; },
    };

    agentgraph->AsyncRun(inputMsg, config);
}

void TEHelper::saveTeamEnvID(std::shared_ptr<SYSAGENT::AccessCtrl> accCtr, std::string envId)
{
    if (accCtr->eventType == SYSAGENT::EventType::TASKASSIGNMENT) {
        accCtr->taskAssignment.header.teamEnvironmentID = envId;
        accCtr->header.teamEnvironmentID = envId;
    } else if (accCtr->eventType == SYSAGENT::EventType::USERINPUT) {
        accCtr->userInput.header.teamEnvironmentID = envId;
        accCtr->header.teamEnvironmentID = envId;
    } else if (accCtr->eventType == SYSAGENT::EventType::TASKSTOP) {
        accCtr->taskStop.header.teamEnvironmentID = envId;
        accCtr->header.teamEnvironmentID = envId;
    } else if (accCtr->eventType == SYSAGENT::EventType::DATAEXCHANGE) {
        accCtr->dataExchange.header.teamEnvironmentID = envId;
        accCtr->header.teamEnvironmentID = envId;
    } else if (accCtr->eventType == SYSAGENT::EventType::USEROUTPUT) {
        accCtr->userOutput.header.teamEnvironmentID = envId;
        accCtr->header.teamEnvironmentID = envId;
    } else if (accCtr->eventType == SYSAGENT::EventType::TASKSTATUSMANAGEMENT) {
        accCtr->taskStatusManagement.header.teamEnvironmentID = envId;
        accCtr->header.teamEnvironmentID = envId;
    } else if (accCtr->eventType == SYSAGENT::EventType::LOGINOUT) {
        accCtr->loginOut.header.teamEnvironmentID = envId;
        accCtr->header.teamEnvironmentID = envId;
    } else if (accCtr->eventType == SYSAGENT::EventType::USERPORTRAITCONF) {
        accCtr->userPortrait.header.teamEnvironmentID = envId;
        accCtr->header.teamEnvironmentID = envId;
    }
}

/**
 * 从 AccessCtrl 消息中提取 TeamEnvironment ID
 * @param data AccessCtrl 消息对象
 * @return 提取的 TeamEnvironment ID，如果无法提取则返回空字符串
 */
std::string TEHelper::extractTEidFromMessage(const std::shared_ptr<SYSAGENT::AccessCtrl>& data)
{
    if (!data) {
        LOG_ERR("Cannot extract TEid: AccessCtrl pointer is invalid");
        return "";
    }

    std::string teamEnvId = "";

    // 根据不同的事件类型从相应的结构中提取 taskID
    switch (data->eventType) {
        case SYSAGENT::EventType::TASKASSIGNMENT: {
            teamEnvId = data->taskAssignment.header.teamEnvironmentID;
            break;
        }
        case SYSAGENT::EventType::TASKSTOP: {
            teamEnvId = data->taskStop.header.teamEnvironmentID;
            break;
        }
        case SYSAGENT::EventType::USERINPUT: {
            teamEnvId = data->userInput.header.teamEnvironmentID;
            break;
        }
        case SYSAGENT::EventType::USEROUTPUT: {
            teamEnvId = data->userOutput.header.teamEnvironmentID;
            break;
        }
        case SYSAGENT::EventType::DATAEXCHANGE: {
            teamEnvId = data->dataExchange.header.teamEnvironmentID;
            break;
        }
        case SYSAGENT::EventType::TASKSTATUSMANAGEMENT: {
            teamEnvId = data->taskStatusManagement.header.teamEnvironmentID;
            break;
        }
        case SYSAGENT::EventType::LOGINOUT: {
            teamEnvId = data->loginOut.header.teamEnvironmentID;
            break;
        }
        case SYSAGENT::EventType::USERPORTRAITCONF: {
            teamEnvId = data->userPortrait.header.teamEnvironmentID;
            break;
        }
        default: {
            LOG_WARN("Unhandled event type: %d, cannot extract teamEnvId", static_cast<int>(data->eventType));
            break;
        }
    }

    if (teamEnvId.empty()) {
        LOG_ERR_EX("Failed to extract teamEnvId from AccessCtrl message with event type: %d",
                   static_cast<int>(data->eventType));
    } else {
        LOG_INFO_EX("Successfully extracted teamEnvId: %s from AccessCtrl", teamEnvId.c_str());
    }

    return teamEnvId;
}

UserInfo TEHelper::extractUserInfo(const std::shared_ptr<SYSAGENT::AccessCtrl>& data)
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
            LOG_INFO_EX("Could not extract user information from unknown event type: %d, using default values",
                        static_cast<int>(data->eventType));
    }

    return userInfo;
}

std::shared_ptr<SYSAGENT::AccessCtrl> TEHelper::newUserOutput(std::string teid, std::string output, std::string status,
                                                              int flag)
{
    auto te = TEManager::getInstance().getTEById(teid);
    if (!te) {
        LOG_ERR_EX("[newUserOutput] TeamEnvironment with ID %s not found", teid.c_str());
        return nullptr;
    }

    const auto& user = te->getUserInfo();
    auto outputMsg = std::make_shared<SYSAGENT::AccessCtrl>();
    outputMsg->eventType = SYSAGENT::EventType::USEROUTPUT;
    auto header = AisfPlat::MsgAisfHeader{
            .messageType = "DataExchange",
            .userName = user.userName,
            .deviceId = user.deviceId,
            .teamEnvironmentID = teid,
            .deviceCategory = user.deviceCategory,
            .uuid = user.uuid,
            .clientSessionId = user.clientSessionId,
            .acmSessionId = user.acmSessionId,
            .eventTimeStamp = AisfPlat::Utils::GetCurrentTimestampInCustomFormat(),
    };
    outputMsg->userOutput.header = header;
    outputMsg->header = header;
    outputMsg->userOutput.data = UserOutput{
            .userOutputText = output,
            .streamStatus = status,
            .userOutPutFlag = flag,
    };

    return outputMsg;
}

std::shared_ptr<SYSAGENT::AccessCtrl> TEHelper::newUserOutput(std::shared_ptr<TeamEnvironment> te, std::string output,
                                                              std::string status, int flag)
{
    if (!te) {
        LOG_ERR_EX("[newUserOutput] TeamEnvironment is null");
        return nullptr;
    }
    const auto& user = te->getUserInfo();
    auto outputMsg = std::make_shared<SYSAGENT::AccessCtrl>();
    outputMsg->eventType = SYSAGENT::EventType::USEROUTPUT;
    auto header = AisfPlat::MsgAisfHeader{
            .messageType = "DataExchange",
            .userName = user.userName,
            .deviceId = user.deviceId,
            .teamEnvironmentID = te->getTeamId(),
            .deviceCategory = user.deviceCategory,
            .uuid = user.uuid,
            .clientSessionId = user.clientSessionId,
            .acmSessionId = user.acmSessionId,
            .eventTimeStamp = AisfPlat::Utils::GetCurrentTimestampInCustomFormat(),
    };
    outputMsg->userOutput.header = header;
    outputMsg->header = header;
    outputMsg->userOutput.data = UserOutput{
            .userOutputText = output,
            .streamStatus = status,
            .userOutPutFlag = flag,
    };

    return outputMsg;
}

std::shared_ptr<SYSAGENT::AccessCtrl> TEHelper::newUserOutput(std::shared_ptr<TeamEnvironment> te,
                                                              std::string outputText, std::string outputObject,
                                                              std::string status, int flag)
{
    if (!te) {
        LOG_ERR_EX("[newUserOutput] TeamEnvironment is null");
        return nullptr;
    }
    const auto& user = te->getUserInfo();
    auto outputMsg = std::make_shared<SYSAGENT::AccessCtrl>();
    outputMsg->eventType = SYSAGENT::EventType::USEROUTPUT;
    auto header = AisfPlat::MsgAisfHeader{
            .messageType = "DataExchange",
            .userName = user.userName,
            .deviceId = user.deviceId,
            .teamEnvironmentID = te->getTeamId(),
            .deviceCategory = user.deviceCategory,
            .uuid = user.uuid,
            .clientSessionId = user.clientSessionId,
            .acmSessionId = user.acmSessionId,
            .eventTimeStamp = AisfPlat::Utils::GetCurrentTimestampInCustomFormat(),
    };
    outputMsg->userOutput.header = header;
    outputMsg->header = header;
    outputMsg->userOutput.data = UserOutput{
            .userOutputText = outputText,
            .userOutputObject = outputObject,
            .streamStatus = status,
            .userOutPutFlag = flag,
    };

    return outputMsg;
}

std::shared_ptr<SYSAGENT::AccessCtrl> TEHelper::newTaskStatusReport(std::string teid, int code, std::string report)
{
    auto te = TEManager::getInstance().getTEById(teid);
    if (!te) {
        LOG_ERR_EX("[newUserOutput] TeamEnvironment with ID %s not found", teid.c_str());
        return nullptr;
    }

    const auto& user = te->getUserInfo();
    auto endMsg = std::make_shared<SYSAGENT::AccessCtrl>();
    endMsg->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    auto header = AisfPlat::MsgAisfHeader{
            .messageType = "TaskStatusManagement",
            .userName = user.userName,
            .deviceId = user.deviceId,
            .teamEnvironmentID = teid,
            .deviceCategory = user.deviceCategory,
            .uuid = user.uuid,
            .clientSessionId = user.clientSessionId,
            .acmSessionId = user.acmSessionId,
            .eventTimeStamp = AisfPlat::Utils::GetCurrentTimestampInCustomFormat(),
    };
    endMsg->header = header;
    endMsg->taskStatusManagement.header = header;
    endMsg->taskStatusManagement.data = TaskStatusManagement{
            .messageType = "TaskStatusReport",
            .taskStatusCode = code,
            .taskStatusReport = report,
    };

    return endMsg;
}

std::shared_ptr<SYSAGENT::AccessCtrl> TEHelper::newTaskStatusReport(std::shared_ptr<TeamEnvironment> te, int code,
                                                                    std::string report)
{
    if (!te) {
        LOG_ERR_EX("[newTaskStatusReport] TeamEnvironment is null");
        return nullptr;
    }
    const auto& user = te->getUserInfo();
    auto endMsg = std::make_shared<SYSAGENT::AccessCtrl>();
    endMsg->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    auto header = AisfPlat::MsgAisfHeader{
            .messageType = "TaskStatusManagement",
            .userName = user.userName,
            .deviceId = user.deviceId,
            .teamEnvironmentID = te->getTeamId(),
            .deviceCategory = user.deviceCategory,
            .uuid = user.uuid,
            .clientSessionId = user.clientSessionId,
            .acmSessionId = user.acmSessionId,
            .eventTimeStamp = AisfPlat::Utils::GetCurrentTimestampInCustomFormat(),
    };
    endMsg->header = header;
    endMsg->taskStatusManagement.header = header;
    endMsg->taskStatusManagement.data = TaskStatusManagement{
            .messageType = "TaskStatusReport",
            .taskStatusCode = code,
            .taskStatusReport = report,
    };

    return endMsg;
}

std::shared_ptr<SYSAGENT::AccessCtrl> TEHelper::newTaskStop(std::shared_ptr<TeamEnvironment> te)
{
    if (te == nullptr) {
        LOG_INFO_EX("te is nullptr");
        return nullptr;
    }
    const auto& user = te->getUserInfo();
    auto endMsg = std::make_shared<SYSAGENT::AccessCtrl>();
    endMsg->eventType = SYSAGENT::EventType::TASKSTOP;
    auto header = AisfPlat::MsgAisfHeader{
            .messageType = "UserTaskManagement",
            .userName = user.userName,
            .deviceId = user.deviceId,
            .teamEnvironmentID = te->getTeamId(),
            .deviceCategory = user.deviceCategory,
            .uuid = user.uuid,
            .clientSessionId = user.clientSessionId,
            .acmSessionId = user.acmSessionId,
            .eventTimeStamp = AisfPlat::Utils::GetCurrentTimestampInCustomFormat(),
    };
    endMsg->header = header;
    endMsg->taskStop.header = header;
    endMsg->taskStop.data = TaskStop{};

    return endMsg;
}

std::shared_ptr<SYSAGENT::AccessCtrl> TEHelper::newUserTaskStop(const std::shared_ptr<TeamEnvironment>& te)
{
    if (te == nullptr) {
        LOG_INFO_EX("te is nullptr");
        return nullptr;
    }
    const auto& user = te->getUserInfo();
    auto endMsg = std::make_shared<SYSAGENT::AccessCtrl>();
    endMsg->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    auto header = AisfPlat::MsgAisfHeader{
            .messageType = "TaskStatusManagement",
            .userName = user.userName,
            .deviceId = user.deviceId,
            .teamEnvironmentID = te->getTeamId(),
            .deviceCategory = user.deviceCategory,
            .uuid = user.uuid,
            .clientSessionId = user.clientSessionId,
            .acmSessionId = user.acmSessionId,
            .eventTimeStamp = AisfPlat::Utils::GetCurrentTimestampInCustomFormat(),
    };
    endMsg->header = header;
    endMsg->taskStatusManagement.header = header;
    endMsg->taskStatusManagement.data.messageType = "TaskStatusReport";
    endMsg->taskStatusManagement.data.taskStatusReport = "TaskCompleted";
    endMsg->taskStatusManagement.data.taskStatusCode = 200;
    return endMsg;
}

void TEHelper::sendUserOutputMsg(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl, string content, string status,
                                 int flag)
{
    std::shared_ptr<TeamEnvironment> te = TEManager::getInstance().getTEById(accessCtrl->header.teamEnvironmentID);
    if (!te) {
        LOG_ERR_EX("[sendUserOutputMsg] TeamEnvironment with ID %s not found",
                   accessCtrl->header.teamEnvironmentID.c_str());
        return;
    }
    auto outputMsg = TEHelper::newUserOutput(te, content, status, flag);  // 结果单独抽取
    if (!outputMsg) {
        LOG_ERR_EX("outputMsg pointer is null");
        return;
    }
    outputMsg->setSender("SystemAgent");
    outputMsg->setReceiver(accessCtrl->extractSender());
    string taskID = accessCtrl->header.taskID; // 注意如果是任务创建，taskID是空的
    outputMsg->setTaskID(taskID);
    outputMsg->setClientExField(accessCtrl->header.clientExField);
    QueueManager::getInstance().postWriteQueue(outputMsg, te->session);
}

void TEHelper::sendUserOutputMsg(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl, string content,
                                 string outputObject, string status, int flag)
{
    std::shared_ptr<TeamEnvironment> te = TEManager::getInstance().getTEById(accessCtrl->header.teamEnvironmentID);
    auto outputMsg = TEHelper::newUserOutput(te, content, outputObject, status, flag); // 结果单独抽取
    if (!outputMsg) {
        LOG_ERR_EX("outputMsg pointer is null");
        return;
    }
    outputMsg->setSender("SystemAgent");
    outputMsg->setReceiver(accessCtrl->extractSender());
    string taskID = accessCtrl->header.taskID; // 注意如果是任务创建，taskID是空的
    outputMsg->setTaskID(taskID);
    outputMsg->setClientExField(accessCtrl->header.clientExField);
    QueueManager::getInstance().postWriteQueue(outputMsg, te->session);
}

void TEHelper::sendNewTaskStatusReport(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl)
{
    std::shared_ptr<TeamEnvironment> te = TEManager::getInstance().getTEById(accessCtrl->header.teamEnvironmentID);
    if (!te) {
        LOG_ERR_EX("[sendNewTaskStatusReport] TeamEnvironment with ID %s not found",
                   accessCtrl->header.teamEnvironmentID.c_str());
        return;
    }
    auto taskMsg = TEHelper::newTaskStatusReport(te, 200, "TaskCompleted");
    if (!taskMsg) {
        LOG_ERR_EX("taskMsg pointer is null");
        return;
    }
    taskMsg->setSender("SystemAgent");
    taskMsg->setReceiver(accessCtrl->extractSender());
    std::string taskID = accessCtrl->header.taskID;
    if (taskID.empty()) {
        LOG_INFO("taskID is empty, post TaskStatusReport Msg");
        QueueManager::getInstance().postWriteQueue(taskMsg, te->session);
        return;
    }
    auto workFlow = te->getWorkFlow(taskID);
    if (workFlow == nullptr) {
        LOG_INFO_EX("workflow 不存在");
        return;
    }
    workFlow->ChangeStatus(taskID, taskMsg);
    // 结束后需要判断workFlow状态，finished需要删除workFlow
    if (workFlow->IsWorkFlowFinished()) {
        LOG_INFO_EX("[CommandTask::exec] workFlow finished, delete it");
        te->deleteWorkFlow();
    }
}

void TEHelper::sendUserTaskCompleted(const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl, const std::string& workflowID)
{
    std::shared_ptr<TeamEnvironment> te = TEManager::getInstance().getTEById(accessCtrl->header.teamEnvironmentID);
    if (!te) {
        LOG_ERR_EX("[sendUserTaskCompleted] TeamEnvironment with ID %s not found",
                   accessCtrl->header.teamEnvironmentID.c_str());
        return;
    }
    auto notifyUserMsg = TEHelper::newUserTaskStop(te);
    notifyUserMsg->setSender("SystemAgent");
    notifyUserMsg->setReceiver(accessCtrl->extractSender());
    auto taskID = workflowID.empty() ? accessCtrl->header.taskID : workflowID;
    notifyUserMsg->setTaskID(taskID);
    notifyUserMsg->setClientExField(accessCtrl->header.clientExField);
    QueueManager::getInstance().postWriteQueue(notifyUserMsg, te->session);
}

std::shared_ptr<WorkFlow> TEHelper::GetWorkFlow(const shared_ptr<AccessCtrl>& accessCtrl)
{
    std::string teamEnvironmentID = accessCtrl->header.teamEnvironmentID;
    if (teamEnvironmentID.empty()) {
        LOG_ERR("can not find teId for message: %s", accessCtrl->toString().c_str());
        return nullptr;
    }
    std::shared_ptr<TeamEnvironment> teamEnvironment = TEManager::getInstance().getTEById(teamEnvironmentID);
    if (teamEnvironment == nullptr) {
        LOG_INFO_EX("[RestCallBack] cannot find TE by TEid");
        return nullptr;
    }
    std::string taskID = accessCtrl->header.taskID;
    shared_ptr<WorkFlow> workFlow = teamEnvironment->getWorkFlow(taskID);
    if (workFlow == nullptr) {
        LOG_ERR("CAN NOT FIND WORKFLOW FOR: %s", accessCtrl->toString().c_str());
        return nullptr;
    }
    return workFlow;
}
