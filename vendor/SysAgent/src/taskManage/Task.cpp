//
// Created by z00913804 on 25-5-20.
//
#include <future>
#include <memory>
#include <utility>

#include "framework_common/utils/env_util.h"

#include "taskManage/Task.h"
#include "callBack/callBack.h"
#include "sys_agent/AgentFlowManager.h"
#include "TeHelper.h"
#include "TeamEnvironment.h"
#include "config/Config.h"
#include "memory/Location.h"
#include "mm_customedreact_llm_component.h"
#include "model/PromptManager.h"
#include "queue/QueueManager.h"
#include "taskManage/QAAgentCallback.h"
#include "taskManage/TeHelper.h"
#include "taskManage/WorkFlow.h"

#include "ruleCognition/CommonDefinition.h"
#define TAIL_LENGTH 2
#include "model/ReactLLMTask.h"
#include <future>

#include "aisf_platform/string/string_utils.h"
#include "securityFence/SecFenceMessage.h"
#include "securityFence/SecurityFence.h"
// Task
void Task::setTaskID(std::string taskId)
{
    taskID = std::move(taskId);
}
TaskStatus Task::getStatus()
{
    return taskStatus;
}
std::string Task::getTaskType()
{
    return taskType;
}
void Task::setStatus(TaskStatus status)
{
    taskStatus = status;
}
std::string Task::findTaskID()
{
    return taskID;
}


bool Task::filterContext(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    std::set<std::string> ignoreAgent = {"StudyBuddyAgent"};
    for (auto agent : ignoreAgent) {
        if (accessCtrl->extractSender() == agent || accessCtrl->extractReceiver() == agent) {
            return false;
        }
    }
    std::string pattern = "<font color=\"gray\" size=\"12\">记忆召回内容: ";
    if (accessCtrl->userOutput.data.userOutputText.find(pattern) != std::string::npos) {
        LOG_INFO_EX("ignore recall memory");
        return false;
    }
    auto taskID = accessCtrl->extractTaskID();
    auto underscoreCount = std::count(taskID.begin(), taskID.end(), '_');
    if (underscoreCount > 1) {
        return false;
    }
    return true;
}

void Task::addQ(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, size_t number)
{
    if (!filterContext(accessCtrl)) {
        return;
    }
    std::string sender = accessCtrl->extractSender();
    std::string receiver = accessCtrl->extractReceiver();
    if (sender == "GUIAgent" || receiver == "GUIAgent") {
        return;
    }
    // 单意图的Q取原始Q
    auto input = (number > 1 || receiver == "HealthAgent") ? requestContent : accessCtrl->extractInput();
    if (accessCtrl->eventType == SYSAGENT::EventType::TASKASSIGNMENT || accessCtrl->eventType == SYSAGENT::EventType::USERINPUT) {
        QAEntity qaEntity {
                .role = "user",
                .message = input,
                .status = "Completed",
                .index = qaIndex ++,
                .date = AisfPlat::Utils::GetCurrentTimeMillisecondStr() + "_" + std::to_string(qaIndex)
        };
        LOG_INFO_EX("%s task add Query:%s", taskID.c_str(), input.c_str());
        queryEntities.push_back(qaEntity);
    }
}

void Task::addA(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    if (!filterContext(accessCtrl)) {
        return;
    }
    std::string sender = accessCtrl->extractSender();
    std::string receiver = accessCtrl->extractReceiver();
    if (sender == "GUIAgent" || receiver == "GUIAgent" || accessCtrl->eventType != SYSAGENT::EventType::USEROUTPUT) {
        return;
    }
    // 判断是否是第一次进入
    if (answerEntities.empty()) {
        QAEntity qaEntity {
                .role = "assistant", .message = accessCtrl->userOutput.data.userOutputText,
                .status = accessCtrl->userOutput.data.streamStatus, .index = qaIndex ++
        };
        answerEntities.push_back(qaEntity);
        LOG_INFO_EX("%s task add Answer insert Finally:%s", taskID.c_str(), qaEntity.message.c_str());
        return;
    }
    auto latestQAEntity = answerEntities[answerEntities.size() - 1];
    LOG_INFO_EX("%s task add Answer:%s", taskID.c_str(), accessCtrl->userOutput.data.userOutputText.c_str());
    if (accessCtrl->userOutput.data.streamStatus == "Running") {
        if (latestQAEntity.status == "Completed") {
            QAEntity qaEntity {
                    .role = "assistant", .message = accessCtrl->userOutput.data.userOutputText,
                    .status = "Running", .index = qaIndex ++
            };
            LOG_INFO_EX("%s task add Answer insert Finally:%s", taskID.c_str(), qaEntity.message.c_str());
            answerEntities.push_back(qaEntity);
        } else if (latestQAEntity.status == "Running") {
            answerEntities.back().message += accessCtrl->userOutput.data.userOutputText;
        }
    } else if (accessCtrl->userOutput.data.streamStatus == "Completed") {
        if (latestQAEntity.status == "Running") {
            answerEntities.back().message += accessCtrl->userOutput.data.userOutputText;
            latestQAEntity.status = "Completed";
        } else if (latestQAEntity.status == "Completed") {
            QAEntity qaEntity {
                    .role = "assistant", .message = accessCtrl->userOutput.data.userOutputText,
                    .status = "Completed", .index = qaIndex ++
            };
            LOG_INFO_EX("%s task add Answer insert Finally:%s", taskID.c_str(), qaEntity.message.c_str());
            answerEntities.push_back(qaEntity);
        }
    }
}

void setSystemAgentIP(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    std::string ip = Utils::GetBaseIp();
    switch (accessCtrl->eventType) {
        case SYSAGENT::EventType::TASKASSIGNMENT:
            accessCtrl->taskAssignment.header.sysAgentEntity = ip;
            break;
        case SYSAGENT::EventType::USERINPUT:
            accessCtrl->userInput.header.sysAgentEntity = ip;
            break;
        case SYSAGENT::EventType::DATAEXCHANGE:
            accessCtrl->dataExchange.header.sysAgentEntity = ip;
            break;
        case SYSAGENT::EventType::TASKSTATUSMANAGEMENT:
            accessCtrl->taskStatusManagement.header.sysAgentEntity = ip;
            break;
        case SYSAGENT::EventType::USEROUTPUT:
            accessCtrl->userOutput.header.sysAgentEntity = ip;
            break;
        case SYSAGENT::EventType::TASKSTOP:
            accessCtrl->taskStop.header.sysAgentEntity = ip;
            break;
            // 添加其他类
        default:
            LOG_INFO_EX("Could not extract user information from unknown event type: %d, using default values", static_cast<int>(accessCtrl->eventType));
    }
}
// 构建一个全新的 报文
std::string ATATask::buildMessageBody(SYSAGENT::AccessCtrl& accessCtrl) const
{
    // 获取正确的header引用
    MsgAisfHeader* header = nullptr;
    switch (accessCtrl.eventType) {
        case SYSAGENT::EventType::TASKASSIGNMENT:
            header = &accessCtrl.taskAssignment.header;
            accessCtrl.taskAssignment.data.userInputObject = data.serialize();
            break;
        case SYSAGENT::EventType::TASKSTOP:
            header = &accessCtrl.taskStop.header;
            break;
        case SYSAGENT::EventType::USERINPUT:
            header = &accessCtrl.userInput.header;
            break;
        case SYSAGENT::EventType::USEROUTPUT:
            header = &accessCtrl.userOutput.header;
            break;
        case SYSAGENT::EventType::DATAEXCHANGE:
            header = &accessCtrl.dataExchange.header;
            break;
        case SYSAGENT::EventType::TASKSTATUSMANAGEMENT:
            header = &accessCtrl.taskStatusManagement.header;
            break;
        default:
            header = &accessCtrl.taskAssignment.header;
            break;
    }

    // 修改基本的Header字段
    if (header) {
        header->taskID = taskID;
        header->sender = "SystemAgent";
    }

    return accessCtrl.toString();
}

// ATATask
ATATask::ATATask(std::string& taskID1, std::shared_ptr<Agent> agent1, std::string& agentName, std::string& requestContent)
{
    taskID = taskID1;
    agent = std::move(agent1);
    this->agentName = agentName;
    this->requestContent = requestContent;
    this->taskStatus = TaskStatus::PENDING;
}

// ATATask
ATATask::ATATask(std::string& taskID1, std::shared_ptr<Agent> agent1, std::string& agentName, std::string& requestContent, const Data& data)
{
    taskID = taskID1;
    agent = std::move(agent1);
    this->agentName = agentName;
    this->requestContent = requestContent;
    this->taskStatus = TaskStatus::PENDING;
    this->data = data;
}

void ATATask::stop(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    LOG_INFO_EX("ATATask [%s] stopped", taskID.c_str());
    // 发送停止请求到 ServiceAgent
    // 设置本 sysAgent 实例的 IP
    setSystemAgentIP(accessCtrl);

    accessCtrl->taskStop.header.taskID = taskID;
    accessCtrl->taskStop.header.receiver = agentName;
    accessCtrl->header = accessCtrl->taskStop.header;
    taskStatus = TaskStatus::FINISHED_BY_USER;
    if (!(agent->sendRestfulRequest(accessCtrl))){
        LOG_INFO_EX("[ATATask::stop] send Stop message failed");
    }
}

bool ATATask::setAgent(std::shared_ptr<Agent> agent1)
{
    if (!agent1) {
        return false;
    }
    agent = agent1;
    return true;
}

bool ATATask::clearAgent()
{
    if (!agent) {
        return false;
    }
    agent = nullptr;
    return true;
}

std::shared_ptr<Agent> ATATask::getAssignedAgent() const
{
    return agent;
}

bool transferOrReturn(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    // AISFDEMO 的消息，除了GUI之外的提示消息不传
    if (accessCtrl->header.sender == "GUIAgent" && accessCtrl->userOutput.data.userOutPutFlag == 1) {
        return false;
    }
    if (accessCtrl->header.receiver == "AISFDEMO" && accessCtrl->userOutput.data.userOutPutFlag != 1) { // DEMOAPP -> AISFDEMO
        return false;
    }
    if (accessCtrl->header.sender != "GUIAgent") {
        return false;
    }
    if (accessCtrl->eventType != SYSAGENT::EventType::USEROUTPUT) {
        return false;
    }
    if (accessCtrl->userOutput.data.userOutPutFlag != 1) {
        return false;
    }
    return true;
}

void ATATask::exec(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    CHECK_NULL_LOG_ERROR_RETURN_VOID(accessCtrl, "[ATATask::exec] accessCtrl is null");
    std::string teamEnvironmentID = accessCtrl->header.teamEnvironmentID;
    std::shared_ptr<TeamEnvironment> teamEnvironment = TEManager::getInstance().getTEById(teamEnvironmentID);
    CHECK_NULL_LOG_ERROR_RETURN_VOID(teamEnvironment, "[ATATask::exec] can not find te by TEID");
    shared_ptr<WorkFlow> workFlow = teamEnvironment->getWorkFlow(taskID);
    CHECK_NULL_LOG_ERROR_RETURN_VOID(workFlow, "[ATATask::exec] workFlow is null");

    LOG_INFO_EX("ATATask [%s] running, agentName:%s taskStatus=%d this=%p",
                taskID.c_str(), agentName.c_str(), static_cast<int>(this->taskStatus), this);
    LOG_INFO_EX("[ATATask::exec] accessCtrl sender=%s receiver=%s eventType=%d msgType=%s",
                accessCtrl->header.sender.c_str(), accessCtrl->header.receiver.c_str(),
                static_cast<int>(accessCtrl->eventType),
                accessCtrl->dataExchange.data.messageType.c_str());
    CHECK_TRUE_LOG_ERROR_RETURN_VOID(transferOrReturn(accessCtrl), "the accessCtrl is a notify message from GUIAgent");
    setSystemAgentIP(accessCtrl);
    accessCtrl->header.clientExField = workFlow->GetClientExField();
    LOG_INFO_EX("ATATASK workflow's clientExField is:%s", workFlow->GetClientExField().c_str());
    // 获取任务的 agent
    shared_ptr<Agent> agent1 = getAssignedAgent();
    // 第一次执行任务时，需要更新报文，使用task里面的 requestContent，
    if (this->taskStatus == TaskStatus::PENDING) { // 第一次执行，
        LOG_INFO_EX("ATATask [%s] first running", taskID.c_str());
        Task::setStatus(TaskStatus::RUNNING);
        if (accessCtrl->taskAssignment.data.userInputObject.empty()) {
            FullData fullData = {200, 200003, "", data};
            accessCtrl->taskAssignment.data.userInputObject = fullData.ToString();
        }
        // 把 requestContent 放入requestContent, 更新一些信息
        accessCtrl->taskAssignment.data.userInputText = requestContent;
        if (isGUIGeneralization) {
            accessCtrl->taskAssignment.data.userSpecifiedAgent = "4";
        }
        if (accessCtrl->header.receiver == "GUIAgent" && accessCtrl->eventType == SYSAGENT::EventType::TASKASSIGNMENT) {
            accessCtrl->taskAssignment.payload = workFlow->GetRootPayLoad(); // GUI消息需要拼上payLoad
            LOG_INFO_EX("GUI message add payload, function is:%s", accessCtrl->taskAssignment.payload.function.c_str());
        }
        accessCtrl->taskAssignment.data.toolName = data.toolName;
        accessCtrl->taskAssignment.data.RewriteQ = data.params["RewriteQ"].empty() ?
                                                   requestContent : data.params["RewriteQ"];
        accessCtrl->taskAssignment.data.isHaveSendHuoShan = data.params["isHaveSendHuoShan"].empty() ? "0" : "1";
        // 保存原始 header 值（后续修改 taskAssignment.header 后仍可读取原始值）
        std::string origTaskID = accessCtrl->header.taskID;
        std::string origTeamEnvID = accessCtrl->header.teamEnvironmentID;
        std::string origSysAgentEntity = accessCtrl->header.sysAgentEntity;
        std::string origSender = accessCtrl->extractSender();
        std::string origClientSessionId = accessCtrl->header.clientSessionId;

        accessCtrl->taskAssignment.data.LongHistory = data.params["LongHistory"];
        accessCtrl->taskAssignment.header.taskID = taskID;
        accessCtrl->taskAssignment.header.receiver = agentName;
        if (data.params.find("receiver") != data.params.end()) {
            accessCtrl->taskAssignment.header.receiver = data.params["receiver"];
        }
        // HealthAgent 分发到 AgentFlow（不走 HTTP agent）
        if (agentName == "HealthAgent") {
            std::vector<std::string> parts = CMFrm::Utils::SplitStr(origTaskID, "_");
            std::string workflowID = parts.empty() ? origTaskID : parts[0];
            std::string dialogueId = "ha_" + workflowID;
            std::string sessionId = origClientSessionId.empty()
                ? workflowID : origClientSessionId;

            // 存上游上下文到 TE extraInfo，供 AgentFlow 响应回调时使用
            auto te = TEManager::getInstance().getTEById(origTeamEnvID);
            if (te) {
                te->setExtraInfo("_ha_sys_agent_entity", origSysAgentEntity);
                te->setExtraInfo("_ha_parent_task_id", origTaskID);
                te->setExtraInfo("_ha_upstream_sender", origSender);
                te->setExtraInfo("_ha_dialogue_id", dialogueId);
                te->setExtraInfo("_ha_session_id", sessionId);
            }

            auto afMgr = SysAgent::AgentFlowManager::GetAgentFlowManager();
            if (!afMgr->SendMsg(accessCtrl, dialogueId, sessionId, "health_agent_rust")) {
                LOG_INFO_EX("[ATATask::exec] HealthAgent AgentFlow SendMsg failed");
            }
            return;
        }

        // GUIAgent 桥接转发到外部 SysAgent（不走本地 agent 注册）
        if (agentName == "GUIAgent") {
            auto te = TEManager::getInstance().getTEById(origTeamEnvID);
            if (!te) {
                LOG_ERR_EX("[ATATask::exec] GUIAgent cannot find TE by ID=%s", origTeamEnvID.c_str());
                return;
            }
            // 存上游上下文，供回调时匹配
            std::vector<std::string> parts = CMFrm::Utils::SplitStr(origTaskID, "_");
            std::string wfid = parts.empty() ? origTaskID : parts[0];
            std::string dialogueId = "ha_" + wfid;
            te->setExtraInfo("_ha_dialogue_id", dialogueId);
            te->setExtraInfo("_ha_parent_task_id", origTaskID);
            te->setExtraInfo("_ha_sys_agent_entity", origSysAgentEntity);
            if (!origClientSessionId.empty()) {
                te->setExtraInfo("_ha_session_id", origClientSessionId);
            }

            // 对标外部 SysAgent ATATask::exec GUIAgent PENDING: userSpecifiedAgent + payload
            isGUIGeneralization = true;
            accessCtrl->taskAssignment.data.userSpecifiedAgent = "4";
            if (accessCtrl->header.receiver == "GUIAgent" && accessCtrl->eventType == SYSAGENT::EventType::TASKASSIGNMENT) {
                accessCtrl->taskAssignment.payload = workFlow->GetRootPayLoad();
                LOG_INFO_EX("[ATATask::exec] GUIAgent add payload, function=%s",
                            accessCtrl->taskAssignment.payload.function.c_str());
            }

            // HTTP/2 POST 到外部 SysAgent
            std::string callbackUrl = "https://" + origSysAgentEntity + ":23767" + USER_REQUEST_URL;
            std::string msgJson = accessCtrl->toString();
            LOG_INFO_EX("[ATATask::exec] GUIAgent bridge forward to external SysAgent: url=%s, msg_len=%zu",
                        callbackUrl.c_str(), msgJson.size());

            auto request = std::make_shared<CMFrm::COM::Http2ClientRequest>();
            request->SetMethod(CMFrm::COM::POST);
            request->SetReqUrl(callbackUrl);
            request->AddHeader("Content-Type", "text/json;charset=utf-8");
            request->SetTimeoutSecond(CMFrm::Constants::Const8);
            request->AddBody(msgJson);
            std::string response;
            if (!SYSAGENT::HttpClient::Htt2SendRequest(request, response)) {
                LOG_ERR_EX("[ATATask::exec] GUIAgent bridge forward failed, url=%s", callbackUrl.c_str());
            } else {
                LOG_INFO_EX("[ATATask::exec] GUIAgent bridge forward success, response=%s", response.c_str());
            }
            return;
        }

        std::string messageBody = accessCtrl->toString();
        // 使用工具函数格式化JSON
        std::string formattedJson = JsonParser::ToPrettyString(messageBody);
        LOG_INFO_EX("Task message body: %s", formattedJson.c_str());
        std::cout << "Task message body: \n" << formattedJson << std::endl;
        if (!(agent1->sendRestfulRequest(accessCtrl))) {
            LOG_INFO_EX("[ATATask::exec] send exec message failed");
        }
        return;
    }

    // GUIAgent RUNNING 状态：GUI 回调到达，路由到 coordinator
    // 对标外部 SysAgent ATATask::exec RUNNING: sender=GUIAgent → 路由结果到父 task
    if (agentName == "GUIAgent") {
        accessCtrl->header.clientExField = "gui_callback";
        std::string dialogueId = teamEnvironment->getExtraInfo("_ha_dialogue_id");
        std::string sessionId = teamEnvironment->getExtraInfo("_ha_session_id");
        if (dialogueId.empty()) {
            LOG_ERR_EX("[ATATask::exec] GUIAgent RUNNING: cannot find dialogueId for TE=%s",
                       teamEnvironment->getTeamId().c_str());
            return;
        }
        LOG_INFO_EX("[ATATask::exec] GUIAgent RUNNING: routing callback to coordinator did=%s", dialogueId.c_str());
        auto afMgr = SysAgent::AgentFlowManager::GetAgentFlowManager();
        if (!afMgr->SendMsg(accessCtrl, dialogueId, sessionId, "health_agent_rust")) {
            LOG_ERR_EX("[ATATask::exec] GUIAgent SendMsg failed");
        }
        return;
    }

    if (accessCtrl->header.sender == "APP" || accessCtrl->header.sender == "LTServer"
    || accessCtrl->header.sender == "AISFDEMO" || accessCtrl->header.sender == "STB") { // 说明是用户输入，无需更新requestContent
        LOG_INFO_EX("ATATask sender APP or LTServer : [%s]", accessCtrl->header.receiver.c_str());
        if (!(agent1->sendRestfulRequest(accessCtrl))) {
            LOG_INFO_EX("[ATATask::exec] send exec message failed");
        }
        return;
    }
    // 进行双发
    if (accessCtrl->header.sender == "GUIAgent" && accessCtrl->header.receiver == "LTServer") {
        LOG_INFO_EX("enter GUISTART msg");
        if (accessCtrl->header.clientExField.empty()) {
            accessCtrl->header.clientExField = workFlow->GetClientExField();
        }
        QueueManager::getInstance().postWriteQueue(accessCtrl, teamEnvironment->session);
        shared_ptr<SYSAGENT::AccessCtrl> newAccessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
        newAccessCtrl->eventType = SYSAGENT::EventType::USEROUTPUT;
        newAccessCtrl->userOutput.header.acmSessionId = accessCtrl->header.acmSessionId;
        newAccessCtrl->userOutput.header.clientExField = accessCtrl->header.clientExField;
        newAccessCtrl->userOutput.header.clientSessionId = accessCtrl->header.clientSessionId;
        newAccessCtrl->userOutput.header.deviceCategory = accessCtrl->header.deviceCategory;
        newAccessCtrl->userOutput.header.deviceId = accessCtrl->header.deviceId;
        newAccessCtrl->userOutput.header.eventTimeStamp = accessCtrl->header.eventTimeStamp;
        newAccessCtrl->userOutput.header.messageType = accessCtrl->header.messageType;
        newAccessCtrl->userOutput.header.receiver = "APP";
        newAccessCtrl->userOutput.header.sender = accessCtrl->header.sender;
        newAccessCtrl->userOutput.header.sysAgentEntity = accessCtrl->header.sysAgentEntity;
        newAccessCtrl->userOutput.header.taskID = accessCtrl->header.taskID;
        newAccessCtrl->userOutput.header.teamEnvironmentID = accessCtrl->header.teamEnvironmentID;
        newAccessCtrl->userOutput.header.userName = accessCtrl->header.userName;
        newAccessCtrl->userOutput.header.uuid = accessCtrl->header.uuid;
        newAccessCtrl->header = newAccessCtrl->userOutput.header;

        newAccessCtrl->userOutput.data.implicitOutputFlag = accessCtrl->userOutput.data.implicitOutputFlag;
        newAccessCtrl->userOutput.data.isVoiceBroadcast = accessCtrl->userOutput.data.isVoiceBroadcast;
        newAccessCtrl->userOutput.data.messageType = accessCtrl->userOutput.data.messageType;
        newAccessCtrl->userOutput.data.userOutPutFlag = accessCtrl->userOutput.data.userOutPutFlag;
        newAccessCtrl->userOutput.data.userOutputObject = accessCtrl->userOutput.data.userOutputObject;
        newAccessCtrl->userOutput.data.userOutputText = accessCtrl->userOutput.data.userOutputText;
        QueueManager::getInstance().postWriteQueue(newAccessCtrl, teamEnvironment->session);
        return;
    }

    if (accessCtrl->header.receiver == "APP" || accessCtrl->header.receiver == "LTServer"
    || accessCtrl->header.receiver == "AISFDEMO" || accessCtrl->header.receiver == "STB"
    || accessCtrl->header.receiver == "HomeNetworkServer") { // 发给APP，那说明是 agent 发来的
        LOG_INFO_EX("ATATask receiver : %s", accessCtrl->header.receiver.c_str());
        // 发送给 当前TEID 关联的ACM会话通道, 发给前端APP
        std::string teamEnvironmentID = accessCtrl->header.teamEnvironmentID;
        if (teamEnvironmentID == "") {
            LOG_INFO_EX("[ATATask::exec] teamEnvironmentID is empty");
            return;
        }
        TEManager& teManager = TEManager::getInstance();
        std::shared_ptr<TeamEnvironment> teamEnvironment = teManager.getTEById(teamEnvironmentID);
        if (teamEnvironment == nullptr) {
            LOG_INFO_EX("[ATATask::exec] cannot find TE by TEid");
            return;
        }
        // 发起
        QueueManager::getInstance().postWriteQueue(accessCtrl, teamEnvironment->session);
        return;
    }
    // 那说明是一个 agent 发给 另一个agent，
    // 直接转发，可能是 FoodAgent -> GUIAgent， 任务是Food->GUI,agentName == GUIAgent 与 receiver 一致
    if (accessCtrl->header.receiver == agentName) {
        LOG_INFO_EX("ATATask Receiver == agentName: %s", agentName.c_str());
        if (!(agent1->sendRestfulRequest(accessCtrl))) {
            LOG_INFO_EX("[ATATask::exec] send exec message failed");
        }
    } else {
        LOG_INFO_EX("ATATask Receiver == parentTask.agentName");
        // 否则，receiver 不是当前任务的 taskAgent 说明是 发给父任务的，比如 GUIAgent 发给 FoodAgent的
        // GUI执行完成了，发送消息给父任务，但是根据报文里面的 SysAgentEntity实例，只能发给sysAgent，再转发给父任务
        // 而且这个任务本身就是 父任务比如FoodAgent提给GUI的，agentName == GUIAgent，而AccessCtrl是GUIAgent发给FoodAgent，不一致
        // 就调用父任务的Agent.sendmsg
        std::vector<std::string> indexArray = CMFrm::Utils::SplitStr(taskID, "_");
        std::string parentTaskID;
        for (uint32_t i = 0; i < indexArray.size() - 1; ++i) { // uuid_1_2
            parentTaskID += indexArray[i] + "_";
        }
        parentTaskID = parentTaskID.substr(0, parentTaskID.length() - 1); // uuid_1)
        LOG_INFO_EX("parentTaskID : %s", parentTaskID.c_str());
        shared_ptr<TaskNode> taskNode1 = workFlow->FindTaskNodeByTaskID(parentTaskID);
        if (taskNode1 == nullptr) {
            LOG_INFO_EX("cannot find parentTask: %s", parentTaskID.c_str());
            return;
        }
        LOG_INFO_EX("[ATATask::exec] found parentTaskNode, taskNode1=%p taskNode1->task=%p",
                    taskNode1.get(), taskNode1->task.get());
        shared_ptr<ATATask> ataTask = std::dynamic_pointer_cast<ATATask>(taskNode1->task);
        if (ataTask == nullptr) {
            LOG_ERR_EX("[ATATask::exec] dynamic_pointer_cast<ATATask> failed! parentTask=%s taskNode1->task type is not ATATask",
                       parentTaskID.c_str());
            return;
        }
        LOG_INFO_EX("[ATATask::exec] ataTask=%p ataTask->agent=%p", ataTask.get(), ataTask->agent.get());
        if (ataTask->agent == nullptr) {
            LOG_ERR_EX("[ATATask::exec] ataTask->agent is null! parentTask=%s", parentTaskID.c_str());
            return;
        }
        if (!(ataTask->agent->sendRestfulRequest(accessCtrl))) {
            LOG_INFO_EX("[ATATask::exec] send exec message failed");
        }
    }
}

// RouteTask

RouteTask::RouteTask(std::string& taskID, std::string& agentName, std::string& requestContent)
{
    this->taskID = taskID;
    this->agentName = agentName;
    this->requestContent = requestContent;
    this->taskStatus = TaskStatus::PENDING;
}

void RouteTask::exec(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{ // 只会进入执行一次
    LOG_INFO_EX("RouteTask [%s] running", taskID.c_str());
    QueueManager::getInstance().EventMark("info", "RouteTask.enter", "success", "enter RouteTask", accessCtrl);

    Task::setStatus(TaskStatus::RUNNING);
    // 发送给当前TEID 关联的ACM会话通道, 发给前端APP
    std::string teamEnvironmentID = accessCtrl->header.teamEnvironmentID;
    if (teamEnvironmentID == "") {
        LOG_INFO_EX("[RouteTask::exec] teamEnvironmentID is empty");
        return;
    }
    TEManager& teManager = TEManager::getInstance();
    std::shared_ptr<TeamEnvironment> teamEnvironment = teManager.getTEById(teamEnvironmentID);
    if (teamEnvironment == nullptr) {
        LOG_INFO_EX("[RouteTask::exec] cannot find TE by TEid");
        return;
    }
    // 构造拒答报文
    shared_ptr<SYSAGENT::AccessCtrl> newAccessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    newAccessCtrl->eventType = SYSAGENT::EventType::USEROUTPUT;

    newAccessCtrl->userOutput.header.messageType = "DataExchange";
    newAccessCtrl->userOutput.header.sender = "AISF";
    newAccessCtrl->userOutput.header.receiver = "LTServer";
    newAccessCtrl->userOutput.header.userName = accessCtrl->header.userName;
    newAccessCtrl->userOutput.header.deviceId = accessCtrl->header.deviceId;
    newAccessCtrl->userOutput.header.teamEnvironmentID = accessCtrl->header.teamEnvironmentID;
    newAccessCtrl->userOutput.header.deviceCategory = accessCtrl->header.deviceCategory;
    newAccessCtrl->userOutput.header.uuid = accessCtrl->header.uuid;
    newAccessCtrl->userOutput.header.clientSessionId = accessCtrl->header.clientSessionId;
    newAccessCtrl->userOutput.header.acmSessionId = accessCtrl->header.acmSessionId;
    newAccessCtrl->userOutput.header.taskID = "";
    newAccessCtrl->userOutput.header.eventTimeStamp = accessCtrl->header.eventTimeStamp;
    newAccessCtrl->userOutput.header.sysAgentEntity = accessCtrl->header.sysAgentEntity;

    newAccessCtrl->userOutput.data.messageType = "UserOutput";
    newAccessCtrl->userOutput.data.userOutputText = "不支持此能力处理";
    newAccessCtrl->userOutput.data.userOutPutFlag = 3;
    newAccessCtrl->userOutput.data.streamStatus = "Completed";
    newAccessCtrl->userOutput.data.isVoiceBroadcast = 1; // 拒答报文不需要语音播报

    newAccessCtrl->header = newAccessCtrl->userOutput.header;

    LOG_INFO_EX("[RouteTask::exec] newAccessCtrl : %s", newAccessCtrl->toString().c_str());
    QueueManager::getInstance().postWriteQueue(newAccessCtrl, teamEnvironment->session);
    // 结束消息，给更新状态信息
    // changeStatus 要构造一个任务上报报文
    shared_ptr<WorkFlow> workFlow = teamEnvironment->getWorkFlow(taskID);
    if (workFlow == nullptr) {
        LOG_INFO_EX("[RouteTask::exec] workFlow nullptr");
        return;
    }
    // 构建任务状态上报报文
    shared_ptr<SYSAGENT::AccessCtrl> taskStatusReportAccessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    taskStatusReportAccessCtrl->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    taskStatusReportAccessCtrl->taskStatusManagement.header = AisfPlat::MsgAisfHeader{
        .messageType = "TaskStatusManagement",
        .sender = "SystemAgent",
        .receiver = "APP",
        .userName = accessCtrl->header.userName,
        .deviceId = accessCtrl->header.deviceId,
        .teamEnvironmentID = accessCtrl->header.teamEnvironmentID,
        .deviceCategory = accessCtrl->header.deviceCategory,
        .uuid = accessCtrl->header.uuid,
        .clientSessionId = accessCtrl->header.clientSessionId,
        .acmSessionId = accessCtrl->header.acmSessionId,
        .taskID = "",
        .eventTimeStamp = accessCtrl->header.eventTimeStamp,
    };
    taskStatusReportAccessCtrl->taskStatusManagement.data = TaskStatusManagement{
        .messageType = "TaskStatusReport",
        .taskStatusCode = 200,
        .taskStatusReport = "TaskCompleted",
    };
    LOG_INFO_EX("[RouteTask::exec] send TaskStatusReport to acm");
    // 需要执行一下去找下一个任务嘛
    LOG_INFO_EX("[RouteTask::exec] taskStatusReportAccessCtrl : %s", taskStatusReportAccessCtrl->toString().c_str());
    workFlow->ChangeStatus(taskID, taskStatusReportAccessCtrl, true);
    // 结束后需要判断workFlow状态，finished需要删除workFlow
    if (workFlow->IsWorkFlowFinished()) {
        LOG_INFO_EX("[RouteTask::exec] workFlow finished, delete it");
        teamEnvironment->deleteWorkFlow();
    }
    QueueManager::getInstance().EventMark("info", "RouteTask.exit", "success", "exit RouteTask", accessCtrl);
}
void RouteTask::stop(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    LOG_INFO_EX("RouteTask [%s] finished", taskID.c_str());
    setStatus(TaskStatus::FINISHED);
    LOG_INFO_EX("RouteTask %s stopped", taskID.c_str());
}

// ModelTask

ModelTask::ModelTask(std::string& taskID, std::string& agentName, std::string& requestContent)
{
    this->taskID = taskID;
    this->agentName = agentName;
    this->requestContent = requestContent;
    this->taskStatus = TaskStatus::PENDING;
}

IntentInfo parseIntentInfo(const std::string &userIntentStr)
{
    IntentInfo intents = {};

    auto intentVec = AisfPlat::StringUtils::Split(userIntentStr, '#');
    for (const auto &it : intentVec) {
        if (it == "9") {
            intents.isDeepThink = true;
        } else if (it == "10") {
            intents.isWithMemory = true;
        } else if (it == "11") {
            intents.isDeepResearch = true;
        } else if (it == "12") {
            intents.isMemDebug = true;
        }
    }

    return intents;
}

void ModelTask::exec(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    LOG_INFO_EX("ModelTask [%s] running", taskID.c_str());
    setStatus(TaskStatus::RUNNING);

    accessCtrl->setTaskID(taskID);
    const auto& userIntent = accessCtrl->eventType == SYSAGENT::EventType::TASKASSIGNMENT ? accessCtrl->taskAssignment.data.userIntent : accessCtrl->userInput.data.userIntent;
    IntentInfo intents = parseIntentInfo(userIntent);

    // 走 sdk 那边的
    LOG_INFO_EX("ModelTask %s exec", taskID.c_str());

    std::cout << "model message: " << accessCtrl->toString() << std::endl;
    // 任务是否结束，结束的话调用 changeStatus
    std::string teamEnvId = TEManager::getInstance().extractTeamEnvId(accessCtrl);
    auto teamEnvironment = TEManager::getInstance().getTEById(teamEnvId);
    if (!teamEnvironment) {
        // todo 执行失败怎么处理
        std::cout << "not fount te: " << teamEnvId << std::endl;
        return;
    }

    // UserTaskManagement
    // DataExchange
    // TaskStatusManagement
    LOG_INFO_EX("last taskid: %s", teamEnvironment->getQATask().c_str());
    if (accessCtrl->header.messageType == "UserTaskManagement") {
        std::cout << "UserTaskManagement" << std::endl;
        if (accessCtrl->eventType == SYSAGENT::EventType::TASKSTOP) {
            std::promise<void> interruptPromise;
            std::future<void> interruptFuture = interruptPromise.get_future();
            mmsdk::MMCustomedLlmComponentExt::GetInstance().InterruptStream(
                teamEnvironment->getQATask(),
                [teamEnvironment, taskID = this->taskID, &interruptPromise](uint32_t errorCode) -> uint32_t {
                LOG_INFO_EX("InterruptStream success");
                auto statusReport = TEHelper::newTaskStatusReport(teamEnvironment->getTeamId(), 200, "TaskCompleted");
                statusReport->setTaskID(taskID);
                statusReport->setSender("SystemAgent");
                statusReport->setReceiver("APP");
                shared_ptr<WorkFlow> workFlow = teamEnvironment->getWorkFlow(taskID);
                    workFlow->ChangeStatus(taskID, statusReport);
                LOG_INFO_EX("send TaskStatusReport to acm");
                interruptPromise.set_value();
                return 0;
                });
            interruptFuture.wait_for(std::chrono::seconds(NUMBER_SIX));
            teamEnvironment->setQALastIntent(accessCtrl->taskAssignment.data.userIntent);
            LOG_INFO_EX("create new Task");
            changeUserIntent(accessCtrl, taskID);
        } else if (accessCtrl->eventType == SYSAGENT::EventType::TASKASSIGNMENT) {
            if (teamEnvironment->getQALastIntent() != accessCtrl->taskAssignment.data.userIntent || teamEnvironment->getQATask() == "-1") {
                teamEnvironment->setQALastIntent(accessCtrl->taskAssignment.data.userIntent);
                LOG_INFO_EX("change Intent or create new task");
                changeUserIntent(accessCtrl, taskID);
            } else {
                auto callback = std::dynamic_pointer_cast<QAAgentCallback>(teamEnvironment->getQACallback());
                callback->setTaskid(taskID);
                sendStreamRequestDispatch(accessCtrl, taskID, intents);
            }
        } else {
            LOG_ERR_EX("invalid message type");
        }
    } else if (accessCtrl->header.messageType == "DataExchange") {
        std::cout << "DataExchange" << std::endl;
        if (teamEnvironment->getQALastIntent() != accessCtrl->userInput.data.userIntent || teamEnvironment->getQATask() == "-1") {
            std::cout << "change Intent" << std::endl;
            teamEnvironment->setQALastIntent(accessCtrl->userInput.data.userIntent);
            changeUserIntent(accessCtrl, taskID);
        } else {
            auto callback = std::dynamic_pointer_cast<QAAgentCallback>(teamEnvironment->getQACallback());
            callback->setTaskid(taskID);
            sendStreamRequestDispatch(accessCtrl, taskID, intents);
        }
    } else if (accessCtrl->header.messageType == "TaskStatusManagement") {
        std::cout << "TaskStatusManagement" << std::endl;

        std::string teamEnvironmentID = accessCtrl->header.teamEnvironmentID;

        std::shared_ptr<TeamEnvironment> teamEnvironment = TEHelper::GetOrCreateTE(accessCtrl, nullptr);
        // 结束消息，给更新状态信息
        // changeStatus
        shared_ptr<WorkFlow> workFlow = teamEnvironment->getWorkFlow(taskID);
        if (workFlow == nullptr) {
            LOG_INFO_EX("workflow is null");
            return;
        }
        workFlow->ChangeStatus(taskID, accessCtrl);
        return;
    }

    std::cout << "model task exec end" << std::endl;
}
void ModelTask::stop(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    LOG_INFO_EX("ModelTask [%s] Stop", taskID.c_str());
    setStatus(TaskStatus::FINISHED);
    // log
    LOG_INFO_EX("ModelTask %s stopped", taskID.c_str());
}

void createStreamTask(std::string sumModel, std::string toolModel,
                      std::string sumModelService, std::string toolModelService,
                      std::shared_ptr<mmsdk::MMLLMAsyncCallBack> streamCallback,
    std::function<uint32_t(uint32_t errorCode, std::string& taskId)> callback)
{
    LOG_INFO_EX("[HandlerManager::generateMMAgentTask] create MMAgentTask begin");
    auto reactLlmComp = std::make_shared<mmsdk::MMCustomedReactLlmComponent>();
    mmsdk::MMReasonParam reasonParam = {.modelServiceName = sumModelService, .modelName = sumModel,
                                        .temperature = 0.5, .topP = 0.95, .maxToken = 1024
    };

    auto ret = reactLlmComp->CreateTask(reasonParam, streamCallback, callback);
    if (ret != 0) {
        std::cout << "create task failed: " << ret << std::endl;
        return;
    }
}

std::string getLocation(const std::string& deviceId){
    auto location = std::make_shared<Location>(deviceId, "", "");
    auto res = location->Query();
    if(res.empty()) {
        return "";
    }
    return res[LOCATION_POS];
}

// 聊天入口，判断是否需要携带记忆
void sendStreamRequestDispatch(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, std::string taskId, const IntentInfo& userIntent)
{
    std::string teamEnvId = TEManager::getInstance().extractTeamEnvId(accessCtrl);
    auto teamEnvironment = TEManager::getInstance().getTEById(teamEnvId);
    CHECK_NULL_LOG_ERROR_RETURN(teamEnvironment, void(), "GET te failed");
    std::string userId = teamEnvironment->getUserInfo().userName;
    std::string query = (accessCtrl->eventType == SYSAGENT::EventType::TASKASSIGNMENT) ?
            accessCtrl->taskAssignment.data.userInputText : accessCtrl->userInput.data.userInputText;
    std::string deviceId = accessCtrl->header.deviceId;
    std::string sender = accessCtrl->header.sender;

    auto qaTask = teamEnvironment->getQATask();
    // 如果不需要携带记忆，则走原流程
    if (!userIntent.isWithMemory) {
        sendStreamRequest(qaTask, query, userIntent, deviceId);
        return;
    }

    // 记忆召回
    RecallMemoriesAsync(query,
                        [qaTask, query, userIntent, deviceId, accessCtrl]
                        (int code, const std::vector<std::string>& memories) {
                        std::ostringstream memoryMirix;

                        memoryMirix << "<font color=\"gray\" size=\"12\">" << "记忆召回内容: ";

                        std::ostringstream oss;
                        // 用户问答内容部分
                        oss << "用户问题：" << query << "\n";
                        // 历史记忆部分
                        oss << "用户历史记忆偏好：";
                        std::string outCallStr = "";
                        if (memories.empty()) {
                            // test 返回mirix召回的记忆内容
                            outCallStr = "无";
                            oss << "NULL";
                        } else {
                            for (size_t i = 0; i < memories.size(); ++i) {
                                oss << "\n" << (i + 1) << ". " << memories[i];
                            }
                            // test 返回mirix召回的记忆内容
                            outCallStr = memories[0];
                        }

                        if (outCallStr == "None") {
                            outCallStr = "无";
                        }

                        memoryMirix << outCallStr << "</font>";

                        if (userIntent.isMemDebug) {
                            TEHelper::sendUserOutputMsg(accessCtrl, memoryMirix.str(), UserOutput_Completed, 0);
                        }

                        sendStreamRequest(qaTask, oss.str(), userIntent, deviceId);
                    },
                        userId, accessCtrl->header.taskID, NUMBER_TWO);
}

void sendStreamRequest(std::string taskid, std::string query, const IntentInfo &userIntent, const std::string& deviceId)
{
    auto reactLlmComp = std::make_shared<mmsdk::MMCustomedReactLlmComponent>();
    // 判断是否需要深度思考
    if (!userIntent.isDeepThink) {
        query = query + "/nothink";
    }
    LOG_INFO_EX("query is : [%s]", query.c_str());
    auto intention = MMUserIntention{.query = query, .modaltype = MMReasonModalType::MMLLM_MODAL_TYPE_TEXT};

    std::string promptKey = "personal_assistant";
    if (userIntent.isWithMemory) {
        promptKey = "personal_assistant_with_memory";
    }
    LOG_INFO_EX("promptKey : [%s]", promptKey.c_str());

    auto tmpl = PromptManager::GetInstance().GetPormptTmpl(promptKey);
    tmpl.RenderPrompt({{"current_date", today_date()}, {"current_location", getLocation(deviceId)}});
    auto req = mmsdk::MMLLMRequest{.intentions = {intention}, .prompt = {tmpl}};

    std::vector<MMToolInfo> tools;
    std::string toolFlag = ConfigManager::getInstance().getEnvInfo("TOOL_FLAG", "false");
    if (toolFlag == "true") {
        for (const auto& tool : ConfigManager::getInstance().getToolList()) {
            tools.push_back(mmsdk::MMToolInfo{
                .key = mmsdk::MMToolKey{.name = tool.id, .owner = "SYSAgent"},
            });
        }
    }
    auto ret = reactLlmComp->RequestAsync(taskid, req, tools);
    LOG_INFO_EX("send request result: %d", ret);
}

void changeUserIntent(std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, std::string& modelTaskId)
{
    auto teid = TEManager::getInstance().extractTeamEnvId(accessCtrl);
    auto teamEnvironment = TEManager::getInstance().getTEById(teid);
    CHECK_NULL_LOG_ERROR_RETURN_VOID(teamEnvironment, "teamEnvironment is null");
    auto reactLlmComp = std::make_shared<mmsdk::MMCustomedReactLlmComponent>();
    // 删除旧的task
    reactLlmComp->DestroyTask(teamEnvironment->getQATask(), [reactLlmComp, teamEnvironment, accessCtrl](uint32_t errorCode, std::string taskId) -> uint32_t {
        std::cout << "destory task: " << taskId << ", code " << errorCode << std::endl;
        return 0;
    });
    const auto& userIntent = accessCtrl->eventType == SYSAGENT::EventType::TASKASSIGNMENT ? accessCtrl->taskAssignment.data.userIntent : accessCtrl->userInput.data.userIntent;
    const auto& request = accessCtrl->eventType == SYSAGENT::EventType::TASKASSIGNMENT ? accessCtrl->taskAssignment.data.userInputText : accessCtrl->userInput.data.userInputText;
    LOG_INFO_EX("create task begin");
    std::shared_ptr<QAAgentCallback> agentCallback = std::make_shared<QAAgentCallback>();
    agentCallback->setTeid(teamEnvironment->getTeamId());
    agentCallback->setTaskid(accessCtrl->header.taskID);
    teamEnvironment->setQACallback(agentCallback);
    std::string toolModelName = ConfigManager::getInstance().getEnvInfo("TOOL_MODEL_NAME", "Qwen3-32B");
    std::string summModelName = ConfigManager::getInstance().getEnvInfo("SUMM_MODEL_NAME", "Qwen3-32B");
    std::string toolModelServiceName = ConfigManager::getInstance().getEnvInfo("TOOL_MODEL_SERVICE_NAME",
                                                                               "llmlargeservice");
    std::string summModelServiceName = ConfigManager::getInstance().getEnvInfo("SUMM_MODEL_SERVICE_NAME",
                                                                               "llmlargeservice");
    LOG_INFO_EX("tool: %s, sum: %s", toolModelName.c_str(), summModelName.c_str());

    IntentInfo intents = parseIntentInfo(userIntent);
    if (intents.isDeepThink) {
        LOG_INFO_EX("userIntent:%s, deepseek", userIntent.c_str());
        teamEnvironment->thinkFlag = true;
    } else {
        LOG_INFO_EX("userIntent:%s, qwen", userIntent.c_str());
        teamEnvironment->thinkFlag = false;
    }
    createStreamTask(summModelName, toolModelName,
                     summModelServiceName, summModelServiceName,
                     agentCallback,
                     [intents, request, accessCtrl, teamEnvironment]
                             (uint32_t errorCode, std::string& taskId) -> uint32_t {
                         if (errorCode != 0) {
                             std::cout << "create failed" << std::endl;
                             return 1;
                         }
                         teamEnvironment->setQATask(taskId);
                         std::cout << "send req" << std::endl;
                         sendStreamRequestDispatch(accessCtrl, taskId, intents);
                         return 0;
                     });
    return;
}

void addUerChatMessage(const std::string& userid, const std::string& clientSessionId, const std::string& content)
{
    AddMemoryHistory(userid,
        ChatMessage{.sessionId = clientSessionId,
            .content = ChatMessageContent{
                .role = "user",
                .content = content,
            }});
}

CommandTask::CommandTask(std::string& taskID, std::string& agentName,
                         std::string& requestContent, std::string& intentPath, Data& data1)
{
    this->taskID = taskID;
    this->agentName = agentName;
    this->requestContent = requestContent;
    this->intentPath = intentPath;
    this->data = data1;
    setStatus(TaskStatus::PENDING);
}

CommandTask::CommandTask(std::string& taskID, std::string& agentName,
                         std::string& requestContent, std::string& intentPath, Data& data1, bool paramsMatch)
{
    this->taskID = taskID;
    this->agentName = agentName;
    this->requestContent = requestContent;
    this->intentPath = intentPath;
    this->data = data1;
    this->paramsMatch = paramsMatch;
    setStatus(TaskStatus::PENDING);
}
/**
 *
 * @param response <think>\n</think>\n\n{{result}}
 * @return
 */
std::string getCleanResponse(std::string response) {
    std::string result = AisfPlat::StringUtils::Strip(trimThinkFlag(response));
    LOG_INFO_EX("去掉think标签后的响应 ：[%s]", result.c_str());
    // 下面去掉可能存在 前置 ```json 和 后置 ```
    std::string startFence = "```json";
    std::string endFence = "```";
    // 找到第一个出现的 startFence
    size_t startPos = result.find(startFence);
    if (startPos == std::string::npos) {
        return result;
    }
    // 找到第一个出现的 endFence（在 startFence 之后）
    size_t endPos = result.find(endFence, startPos + startFence.length());
    if (endPos == std::string::npos) {
        return result;
    }
    // 提取中间的内容
    result = result.substr(startPos + startFence.length(), endPos - (startPos + startFence.length()));
    return AisfPlat::StringUtils::Strip(result);
}

std::map<std::string, std::string> getParamsMap(std::string response) {
    std::map<std::string, std::string> params;
    // 1. 创建 JSON 解析器
    Json::CharReaderBuilder builder;
    // 2. 创建输入字符串流
    std::istringstream inputStream(response);
    // 3. 创建根节点对象和错误信息变量
    Json::Value root;               // 解析后的 JSON 对象
    JSONCPP_STRING errs;            // 保存解析错误信息
    // 4. 使用 CharReader 解析 JSON
    if (!Json::parseFromStream(builder, inputStream, &root, &errs)) {
        std::cerr << "解析 JSON 失败：" << errs << std::endl;
        return params;
    }
    for (const auto& name : root.getMemberNames()) {
        params[name] = root[name].asString();
    }
    return params;

}

// 模型提参 小模型 Qwen3的0.6B和1.7B
void CommandTask::modelExtractParams(string modelName, string modelServiceName,
                                     const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl,
                                     const string& requestContent, string paramsJsonStr)
{
    // 调用模型提参
    auto llmComponent = std::make_shared<mmsdk::MMLlmComponent>();
    MMReasonParam reasonParam = {.modelServiceName = modelServiceName, .modelName = modelName,
                                 .temperature = 0.001, .maxToken = 1024
    };
    string userPrompt = "##用户输入\n" + requestContent + "/nothink";
    auto teamEnvironment = TEManager::getInstance().getTEById(accessCtrl->header.teamEnvironmentID);
    auto intention = MMUserIntention{.query = userPrompt, .modaltype = MMReasonModalType::MMLLM_MODAL_TYPE_TEXT};
    auto systemPromptTmplt =
            MMPromptTemplate(MMPromptTmpltKey{.name = "parameter_extraction", .owner = "SYSAgent", .version = "0.1"});
    auto promptInfoList = ConfigManager::getInstance().getPromptInfoList();
    for (const auto& promptInfo : promptInfoList) {
        if (promptInfo.name == "parameter_extraction") {
            systemPromptTmplt.SetContent(promptInfo.promptTemplate);
        }
    }
    systemPromptTmplt.RenderPrompt({{"params_list", paramsJsonStr}});
    LOG_INFO_EX("PrintDebugInfo, 准备发送提参请求");
    LOG_INFO_EX("userPrompt [%s], systemPrompt [%s]", userPrompt.c_str(), systemPromptTmplt.GetContent().c_str());
    auto req = MMLLMRequest{.intentions = {intention}, .prompt = {systemPromptTmplt}};
    auto start = std::make_shared<std::chrono::time_point<std::chrono::steady_clock>>(std::chrono::steady_clock::now());
    QueueManager::getInstance().EventMark("info", "CommandTask.ModelExtractParamsBegin", "success",
                                          "start to invoke model", accessCtrl);
    auto ret = llmComponent->RequestAsync(reasonParam, req,
                                          [start, accessCtrl, teid = teamEnvironment->getTeamId(), this]
    (uint32_t errorCode, const std::string& response) -> uint32_t {
        auto end = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - *start).count();
        LOG_INFO_EX("PrintDebugInfo : modelExtractParams use count: %ld", ms);
        LOG_INFO_EX("PrintDebugInfo, extract params errorCode: %d, response: %s", errorCode, response.c_str());
        std::string result = errorCode == 0 ? "success" : "failed";
        QueueManager::getInstance().EventMark("info", "CommandTask.ModelExtractParamsEnd", result,
                                              "callback received msg, invoke model result : " + result, accessCtrl);
        auto te = TEManager::getInstance().getTEById(teid);
        if (!te) {
            LOG_ERR_EX("Failed to get te");
            return 1;
        }
        if (errorCode != mmsdk::SUCCESS) {
            TEHelper::sendUserOutputMsg(accessCtrl, "提参模型调用失败", UserOutput_Completed, 0);
            TEHelper::sendNewTaskStatusReport(accessCtrl);
            return 1;
        }
        string res = getCleanResponse(response);
        // 转成 map
        std::map<string, string> respMap = getParamsMap(res);
        data.params = respMap;
        FullData fullData = {
              200,
              200001,
              "操作成功",
              data
        };
        TEHelper::sendUserOutputMsg(accessCtrl, "", fullData.ToString(), UserOutput_Completed, 0);
        TEHelper::sendNewTaskStatusReport(accessCtrl);
        return 0;
    });
    if (ret != mmsdk::SUCCESS) {
        QueueManager::getInstance().EventMark("error", "CommandTask.ModelExtractParamsEnd", "failed",
                                              "RequestAsync invoke model return result : failed", accessCtrl);
        TEHelper::sendUserOutputMsg(accessCtrl, "提参模型调用失败", UserOutput_Completed, 0);
        TEHelper::sendNewTaskStatusReport(accessCtrl);
    }
    QueueManager::getInstance().EventMark("info", "CommandTask.ModelExtractParamsEnd", "success",
                                          "RequestAsync invoke model return result : success", accessCtrl);
}
string getPromptByIntentName()
{
    LOG_INFO_EX("getPromptByIntentName enter ...");
    string prompt;
    auto promptInfoList = ConfigManager::getInstance().getPromptInfoList();
    for (const auto& promptInfo : promptInfoList) {
        if (promptInfo.name == "parameter_decision") {
            prompt = promptInfo.promptTemplate;
        }
    }
    LOG_INFO_EX("getPrompt [%s]", prompt.c_str());
    return prompt;
}

void parseCleanResponse(std::string cleanResponse, std::string& intentName, std::map<std::string, std::string>& params) {
    Json::CharReaderBuilder builder;
    std::istringstream inputStream(cleanResponse);
    Json::Value root;               // 解析后的 JSON 对象
    JSONCPP_STRING errs;            // 保存解析错误信息
    if (!Json::parseFromStream(builder, inputStream, &root, &errs)) {
        std::cerr << "解析 JSON 失败：" << errs << std::endl;
    }
    intentName = root["function"].asString();
    std::cout << "intentName: " << intentName << std::endl;
    for (const auto& name : root["arguments"].getMemberNames()) {
        params[name] = root["arguments"][name].asString();
    }
}

// 模型提参大模型 Qwen3-32B
void CommandTask::extractParamsByLLM(string modelName, string modelServiceName,
                                     const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl,
                                     const string& requestContent)
{
    auto llmComponent = std::make_shared<mmsdk::MMLlmComponent>();
    MMReasonParam reasonParam = {.modelServiceName = modelServiceName, .modelName = modelName,
                                 .temperature = 0.001, .maxToken = 1024
    };
    string userPrompt = "##用户输入\n" + requestContent + "/nothink";
    auto teamEnvironment = TEManager::getInstance().getTEById(accessCtrl->header.teamEnvironmentID);
    auto intention = MMUserIntention{.query = userPrompt, .modaltype = MMReasonModalType::MMLLM_MODAL_TYPE_TEXT};
    auto systemPromptTmplt =
            MMPromptTemplate(MMPromptTmpltKey{.name = "parameter_extraction_LLM", .owner = "SYSAgent", .version = "0.1"});
    LOG_INFO_EX("data category : [%s]", data.category.c_str());
    std::map<std::string, std::string> categoryToPromptMap = ConfigManager::getInstance().getCategoryToPromptMap();
    systemPromptTmplt.SetContent(categoryToPromptMap[data.category]);
    LOG_INFO_EX("PrintDebugInfo, 准备发送大模型提参请求"); // 打印时间
    auto start = std::make_shared<std::chrono::time_point<std::chrono::steady_clock>>(std::chrono::steady_clock::now());
    LOG_INFO_EX("userPrompt [%s], systemPrompt [%s]", userPrompt.c_str(), systemPromptTmplt.GetContent().c_str());
    auto req = MMLLMRequest{.intentions = {intention}, .prompt = {systemPromptTmplt}};
    QueueManager::getInstance().EventMark("info", "CommandTask.ExtractParamsByLLMBegin", "success",
                                          "start to invoke llm large model", accessCtrl);
    auto ret = llmComponent->RequestAsync(reasonParam, req,
  [start, accessCtrl, teid = teamEnvironment->getTeamId(), this]
        (uint32_t errorCode, const std::string& response) -> uint32_t {
        auto end = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - *start).count();
        LOG_INFO_EX("PrintDebugInfo, extractParamsByLLM use count: %ld", ms);
        LOG_INFO_EX("PrintDebugInfo, LLM extract params errorCode: %d, response: %s", errorCode, response.c_str());        std::string result = errorCode == 0 ? "success" : "failed";
        QueueManager::getInstance().EventMark("info", "CommandTask.ExtractParamsByLLMEnd", result,
                                              "callback received msg, invoke llm large model result : " + result, accessCtrl);
        auto te = TEManager::getInstance().getTEById(teid);
        if (!te) {
            LOG_ERR_EX("Failed to get te");
            return 1;
        }
        if (errorCode != mmsdk::SUCCESS) {
            TEHelper::sendUserOutputMsg(accessCtrl, "调用大模型提参失败", UserOutput_Completed, 0);
            TEHelper::sendNewTaskStatusReport(accessCtrl);
            return 1;
        }
        string cleanResponse = getCleanResponse(response);// 去掉think标签和空格
        std::string intentName;
        std::map<string, string> paramsMap;
        parseCleanResponse(cleanResponse, intentName, paramsMap);
        data.intent = intentName;
        data.params = paramsMap;
        FullData fullData = {
              200,
              200001,
              "操作成功",
              data
        };
        TEHelper::sendUserOutputMsg(accessCtrl, "", fullData.ToString(), UserOutput_Completed, 0);
        TEHelper::sendNewTaskStatusReport(accessCtrl);
        return 0;
    });
    if (ret != mmsdk::SUCCESS) {
        QueueManager::getInstance().EventMark("error", "CommandTask.ExtractParamsByLLMEnd", "failed",
                                              "RequestAsync invoke llm large model return result : failed", accessCtrl);
        TEHelper::sendUserOutputMsg(accessCtrl, "调用大模型提参失败", UserOutput_Completed, 0);
        TEHelper::sendNewTaskStatusReport(accessCtrl);
    }
    QueueManager::getInstance().EventMark("info", "CommandTask.ExtractParamsByLLMEnd", "failed",
                                          "RequestAsync invoke model return result : success", accessCtrl);
}

void CommandTask::exec(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    LOG_INFO_EX("CommandTask [%s] running", taskID.c_str());
    QueueManager::getInstance().EventMark("info", "CommandTask.exec.enter", "success", "enter CommandTask", accessCtrl);
    setStatus(TaskStatus::RUNNING);
    LOG_INFO_EX("intentPath : [%s], data.intent : [%s]", intentPath.c_str(), data.intent.c_str());
    auto intentParamsMap = ConfigManager::getInstance().getIntentToParamsString();
    auto intentMap = ConfigManager::getInstance().getIntents();
    if (!data.intent.empty() && intentMap.count(data.intent) == 0) { // 如果intent不空且不在配置里，为空肯走的大模型
        LOG_ERR_EX("cannot find intent [%s] in intentMap", data.intent.c_str());
        return;
    }
    std::unordered_set<std::string> permitIntent{"INTENT_OPEN_APP"};
    if (!data.intent.empty() && (permitIntent.find(data.intent.c_str()) != permitIntent.end() || intentMap.at(data.intent).noArgs)) { // 无参指令, 默认意图识别的结果中意图名称一定是正确的
        LOG_INFO_EX("intent : [%s] have no params", data.intent.c_str());
        FullData fullData = {
                200,
                200001,
                "操作成功",
                data
        };
        TEHelper::sendUserOutputMsg(accessCtrl, "", fullData.ToString(), UserOutput_Completed, 0);
        TEHelper::sendNewTaskStatusReport(accessCtrl);
        return;
    }
    LOG_INFO_EX("intentPath : [%s]", intentPath.c_str());
    if (intentPath == "SerialLModel" || intentPath == "ParallelLModel") { // 有参数慢路径，需要用大模型进行提参 0 rag 1 小模型 2 大模型 3 统一意图
        LOG_INFO_EX("intentPath : [%s], 需要用大模型提参", intentPath.c_str());
        string modelName = ConfigManager::getInstance().getEnvInfo(ENV_PARAMS_EXTRACT_BIG_MODEL, "Qwen3-32B"); // Qwen3-32B
        std::string modelServiceName = ConfigManager::getInstance().getEnvInfo(ENV_PARAMS_EXTRACT_BIG_MODEL_SERVICE_NAME, "llmlargeservice");
        LOG_INFO_EX("use modelName [%s], modelServiceName [%s] to extract params", modelName.c_str(), modelServiceName.c_str());
        extractParamsByLLM(modelName, modelServiceName, accessCtrl, requestContent);
        return;
    }
    if (paramsMatch) {
        LOG_INFO_EX("参数正确，不需要二次提参");
        FullData fullData = {
                200,
                200001,
                "操作成功",
                data
        };
        TEHelper::sendUserOutputMsg(accessCtrl, "", fullData.ToString(), UserOutput_Completed, 0);
        TEHelper::sendNewTaskStatusReport(accessCtrl);
        return;
    } else {
        LOG_INFO_EX("参数错误，进行二次提参");
        string modelName =
                ConfigManager::getInstance().getEnvInfo(ENV_PARAMS_EXTRACT_SMALL_MODEL, "qwen3-4B");  // Qwen3-0.6B or Qwen3-1.7B
        std::string modelServiceName =
                ConfigManager::getInstance().getEnvInfo(ENV_PARAMS_EXTRACT_SMALL_MODEL_SERVICE_NAME, "llmsmallservice");
        LOG_INFO_EX("use modelName [%s], modelServiceName [%s] to extract params", modelName.c_str(), modelServiceName.c_str());
        string paramsJsonStr = ConfigManager::getInstance().getIntentToParamsString()[data.intent];
        modelExtractParams(modelName, modelServiceName, accessCtrl, requestContent, paramsJsonStr);
    }
    QueueManager::getInstance().EventMark("info", "CommandTask.exec.exit", "success", "exit CommandTask", accessCtrl);
}

void CommandTask::stop(shared_ptr<SYSAGENT::AccessCtrl>)
{
    LOG_INFO_EX("CommandTask [%s] finished", taskID.c_str());
    setStatus(TaskStatus::FINISHED);
    LOG_INFO_EX("CommandTask [%s] stopped", taskID.c_str());
}

std::string Data::serialize() const
{
    Json::Value root;
    root["intent"] = intent;
    Json::Value paramsJson;
    for (const auto& param : params) {
        paramsJson[param.first] = param.second;
    }
    root["params"] = paramsJson;

    return JsonParser::ToRawString(root);
}

void Data::deserialize(Data& data, const std::string& jsonStr)
{
    Json::Value root;
    if (!JsonParser::Parse(jsonStr.c_str(), root)) {
        LOG_ERR_EX("call json parse failed: %s", jsonStr.c_str());
        return;
    }
    JsonParser::GetString(root, "intent", data.intent);
    Json::Value paramsJson = root["params"];
    for (const auto& key : paramsJson.getMemberNames()) {
        data.params[key] = paramsJson[key].asString();
    }
}

Json::Value Data::parseJson() {
    Json::Value root;
    root["intent"] = intent;
    Json::Value paramsJson;
    for (const auto& param : params) {
        paramsJson[param.first] = param.second;
    }
    root["params"] = paramsJson;

    return root;
}