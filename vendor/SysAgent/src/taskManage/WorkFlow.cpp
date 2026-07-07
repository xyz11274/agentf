//
// Created by z00913804 on 25-5-19.
//
#include "taskManage/WorkFlow.h"
#include "TeHelper.h"
#include "TeamEnvironment.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "aisf_platform/common_macro.h"
#include "queue/QueueManager.h"
#include "taskManage/Task.h"
#include "taskManage/TaskEntity.h"
#include "taskManage/TaskNode.h"
#include "taskManage/ToolTask.h"
#include "taskManage/OpenclawTask.h"
#include <cstddef>
#include <string>
#include "memory/SharedMemory.h"
#include "memory/ContextManager.h"
#include "aisf_platform/utils/utils.h"
#include "registerManage/AgentManager.h"
#include "model/PromptManager.h"
#include <random>

// workflowID 自己 生成 generateRandomWorkFlowID()
WorkFlow::WorkFlow(std::string workflowID)
{
    LOG_INFO("Create workflow with ID %s", workflowID.c_str());
    std::string taskID = workflowID.empty() ? GenerateRandomWorkFlowID() : workflowID;
    this->workFlowID = taskID;
    workFlowStatus = WorkFlowStatus::PENDING;
    UpdateWorkFlowLiveTime();
}

// 获取agent实例失败，创建的任务就失败，对于IntentTask，返回给前端提示消息结果报文和任务结束报文，SubTask，只发送任务结束报文
void WorkFlow::SendTaskCreateFailedMessage(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, string workFlowIDorParentTaskID, string taskType) {
    LOG_INFO_EX("准备按任务类型发送任务创建失败状态上报消息给前端/父任务Agent, taskType ： %s", taskType.c_str());
    // 任务创建失败，给前端APP返回结果报文，taskID变成workFlowID
    TEManager& teManager = TEManager::getInstance();
    std::string teamEnvId = accessCtrl->header.teamEnvironmentID;
    if (teamEnvId.empty()) {
        LOG_INFO_EX("[WorkFlow::ChangeStatus] teamEnvironmentID is empty");
        return;
    }
    std::shared_ptr<TeamEnvironment> teamEnvironment = teManager.getTEById(teamEnvId);
    if (teamEnvironment == nullptr) {
        LOG_INFO_EX("cannot find TE by id");
        return;
    }
    // 构建任务状态上报消息
    shared_ptr<SYSAGENT::AccessCtrl> taskStatuReportMsg = TEHelper::newTaskStatusReport(teamEnvironment, 200, "TaskCompleted");
    if (taskStatuReportMsg == nullptr) {
        LOG_ERR_EX("Failed to get te, build taskStatuReportMsg failed");
    }
    taskStatuReportMsg->setSender("SystemAgent");
    taskStatuReportMsg->setReceiver(sender);
    taskStatuReportMsg->setTaskID(workFlowIDorParentTaskID);
    taskStatuReportMsg->setClientExField(clientExField);
    taskStatuReportMsg->setSystemAgentIP();
    if (taskType == "IntentTask") {
        QueueManager::getInstance().postWriteQueue(taskStatuReportMsg, teamEnvironment->session);
        LOG_INFO_EX("创建意图任务失败，发任务状态上报消息给前端");
        return;
    }
    LOG_INFO_EX("创建子任务失败，发任务状态上报消息给父任务");
    // subTask 那么需要发给父任务
    LOG_INFO_EX("parentTaskID == taskEntityTaskID: %s", workFlowIDorParentTaskID.c_str());
    shared_ptr<WorkFlow> workFlow = teamEnvironment->getWorkFlow(workFlowIDorParentTaskID);
    CHECK_NULL_LOG_ERROR_RETURN_VOID(workFlow, "get workflow failed");
    shared_ptr<TaskNode> taskNode = workFlow->GetTaskNodeByTaskID(workFlowIDorParentTaskID);
    if (taskNode == nullptr) {
        LOG_INFO_EX("cannot find parentTask: %s", workFlowIDorParentTaskID.c_str());
        return;
    }
    shared_ptr<ATATask> ataTask = std::dynamic_pointer_cast<ATATask>(taskNode->task);
    // 发给父任务，需要把 receiver 改成父任务的 agent // 好像没必要
    LOG_INFO_EX("父任务agent type ： %s", ataTask->agent->getType().c_str());
    taskStatuReportMsg->taskStatusManagement.header.receiver = ataTask->agent->getType();
    taskStatuReportMsg->header.receiver = ataTask->agent->getType();
    if (!(ataTask->agent->sendRestfulRequest(taskStatuReportMsg))) {
        LOG_ERR_EX("send taskStatuReportMsg failed");
    };
}

bool WorkFlow::InitWorkFlow(std::vector<IntentTask>& intentTasks, shared_ptr<SYSAGENT::AccessCtrl> accessCtrl,
                            std::vector<AgentData>& agentDatas)
{
    auto te = TEManager::getInstance().getTEById(accessCtrl->header.teamEnvironmentID);
    CHECK_TRUE_LOG_ERROR_RETURN(!te, false, "TE is empty!");
    if (intentTasks.empty() && agentTasks.empty()) {
        LOG_INFO_EX("intentTask and agentTask is empty");
        return false;
    }
    CHECK_TRUE_LOG_ERROR_RETURN(agentDatas.empty(), false, "agentDatas is empty, maybe no-ATATask");
    for (const AgentData& agentData : agentDatas) {
        LOG_INFO_EX("agent data : %s", agentData.name.c_str());
    }
    sender = accessCtrl->header.sender;               // 第一次创建workflow的时候，把sender存下来，方便后续使用
    nowSender = sender;
    te->sender = sender;
    nowTaskId = accessCtrl->extractTaskID();
    rootPayLoad = accessCtrl->taskAssignment.payload;
    UpdateWorkFlowLiveTime();
    if (!accessCtrl->header.clientExField.empty()) {
        clientExField = accessCtrl->header.clientExField; // 保存 LTServer和AISF实例通信的关联标识
        te->clientExField = accessCtrl->header.clientExField;
    }
    return true;
}

// tasks [[AgentName, RequestContent]...]
bool WorkFlow::CreateWorkFlowTask(std::vector<IntentTask>& intentTasks, const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl,
                                  std::vector<AgentData>& agentDatas)
{
    if (!InitWorkFlow(intentTasks, accessCtrl, agentDatas)) {
        LOG_INFO_EX("create workflow failed!");
        return false;
    }
    // 创建任务
    for (uint32_t i = 0; i < intentTasks.size(); i++) { // 遍历任务列表, 创建任务节点
        std::string agentName = intentTasks[i].agentName;
        std::string requestContent = intentTasks[i].requestContent;
        std::string app = intentTasks[i].APP;
        std::string intentPath = intentTasks[i].intentPath;
        bool paramsMatch = intentTasks[i].paramsMatch;
        Data data = intentTasks[i].data;
        LOG_INFO_EX("CreateWorkFlowTask data : [%s]", data.serialize().c_str());
        LOG_INFO_EX("intentTask agentName : %s, requestContent : %s, app : %s", agentName.c_str(), requestContent.c_str(), app.c_str());
        if (
                (agentName == "FoodieAgent" && app == "美团") ||
                (agentName == "AnyShoppingAgent" && app == "美团") ||
                (agentName == "FoodieAgent" && app == "京东") ||
                (agentName == "FoodieAgent" && app == "")) {
            agentName = "FoodieAgent";
        } else if (
                (agentName == "AnyShoppingAgent" && app == "淘宝") ||
                (agentName == "AnyShoppingAgent" && app == "京东") ||
                (agentName == "FoodieAgent" && app == "淘宝") ||
                (agentName == "AnyShoppingAgent" && app == "")) {
            agentName = "AnyShoppingAgent";
        }
        app = app == "携程旅行" ? "携程" : app;
        accessCtrl->taskAssignment.data.APPName = app; // 把APPName存报文，方便后续使用
        std::string taskID = workFlowID + "_" + std::to_string(i); // uuid_0, uuid_1, ...
        // 根据 agentName 判断是 RouteTask 、ATATask 、ModelTask
        std::shared_ptr<Task> intentTask;
        if (agentName == "ToolAgent") {
            intentTask = std::make_shared<ToolTask>(taskID, agentName, requestContent, workFlowID);
        } else if (agentName == "RouteAgent") { // routeTask have no agent
            intentTask = std::make_shared<RouteTask>(taskID, agentName, requestContent);
        } else if (agentName == "SystemAgent") { // modelTask have no agent
            intentTask = std::make_shared<ModelTask>(taskID, agentName, requestContent);
        } else if (agentName == "CommandAgent") {
            intentTask = std::make_shared<CommandTask>(taskID, agentName, requestContent, intentPath, data, paramsMatch);
        } else if (agentName == "OpenclawAgent") {
            intentTask = std::make_shared<OpenclawTask>(taskID, agentName, requestContent);
        } else if (agentName == "HealthAgent") {
            intentTask = std::make_shared<ATATask>(taskID, nullptr, agentName, requestContent);
        } else if (agentName == "GUIAgent") {
            intentTask = std::make_shared<ATATask>(taskID, nullptr, agentName, requestContent);
        } else { // 其他 Agent 全是 ATATask
            // 根据规则节点或者意图认知节点传过来的agent信息获取实例
            std::shared_ptr<Agent> agent = Agent::getAgentInstance(agentName, agentDatas);
            if (agent == nullptr) {
                LOG_ERR_EX("getAgentInstance failed");
                return false;
            }
            intentTask = std::make_shared<ATATask>(taskID, agent, agentName, requestContent, data);
            LOG_INFO("[WorkFlow] CreateWorkFlowTask agent id: %s", intentTask->findTaskID().c_str());
        }
        if (intentTask) {
            std::shared_ptr<TaskNode> taskNode = std::make_shared<TaskNode>(intentTask);
            taskNodes.push_back(taskNode);
            LOG_INFO_EX("Add Task to the workflowID %s, taskID %s, agentName %s, requestContent %s", workFlowID.c_str(), taskID.c_str(), agentName.c_str(), requestContent.c_str());
        }
    }
    return true;
}

std::string WorkFlow::CreateSubTask(TaskEntity taskEntity, shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    if (workFlowStatus == WorkFlowStatus::FINISHED_BY_USER || workFlowStatus == WorkFlowStatus::FINISHED) {
        LOG_INFO_EX("cannot CreateSubTask, because the workflow have finished");
        return "";
    }
    // taskEntity 里面的 taskID 一定是已经存在的父任务ID
    // eg: taskID = "abc_1_1_1", indexArray = [abc, 1, 1, 1] , size >= 2
    std::cout << "[WorkFlow::CreateSubTask] use GetTaskNodeByTaskID" << std::endl;
    shared_ptr<TaskNode> parentTaskNode = GetTaskNodeByTaskID(taskEntity.taskID);
    if (parentTaskNode == nullptr) {
        LOG_INFO_EX("cannot find parentTask, fail to create sub task");
        return "";
    }

    // 要根据 agentName 判断属于 routeTask 还是 systemTask
    std::string newTaskID = taskEntity.taskID + "_" + std::to_string(parentTaskNode->childrenNode.size());

    shared_ptr<Task> subTask;
    if (taskEntity.agentName == "SystemAgent") { // localTask has no agent
        // 没有 agent, 只有taskID
        subTask = std::make_shared<ModelTask>(newTaskID, taskEntity.agentName, taskEntity.requestContent);
    } else if (taskEntity.agentName == "RouteAgent") {
        subTask = std::make_shared<RouteTask>(newTaskID, taskEntity.agentName, taskEntity.requestContent);
    } else if (taskEntity.agentName == "HealthAgent") {
        // HealthAgent 是内部 AgentFlow 分发，不需要外部 agent 实例
        subTask = std::make_shared<ATATask>(newTaskID, nullptr, taskEntity.agentName, taskEntity.requestContent);
    } else if (taskEntity.agentName == "GUIAgent") {
        // GUIAgent 桥接转发到外部 SysAgent，不需要本地 agent 实例
        subTask = std::make_shared<ATATask>(newTaskID, nullptr, taskEntity.agentName, taskEntity.requestContent);
    } else { // 其他 Agent 全是 ATATask
        // 根据agentName获取实例
        std::shared_ptr<Agent> agent = Agent::getAgentInstance(taskEntity.agentName);
        if (agent == nullptr) {
            LOG_ERR_EX("getAgentInstance failed, %s", taskEntity.agentName.c_str());
            // 子任务那就发给父任务的agent, 但业务agent暂未做处理，不会返回任务状态上报报文给SysAgent
            SendTaskCreateFailedMessage(accessCtrl, taskEntity.taskID, "SubTask");
            return "";
        }
        subTask = std::make_shared<ATATask>(newTaskID, agent, taskEntity.agentName, taskEntity.requestContent);
    }
    std::shared_ptr<TaskNode> subTaskNode = std::make_shared<TaskNode>(subTask);
    parentTaskNode->childrenNode.push_back(subTaskNode);
    LOG_INFO_EX("Add subtask to the workflowID %s, taskID %s, agentName %s, requestContent %s", workFlowID.c_str(), newTaskID.c_str(), taskEntity.agentName.c_str(),
                taskEntity.requestContent.c_str());
    return newTaskID;
}

void WorkFlow::StopSubTasks(const shared_ptr<TaskNode>& taskNode, const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl)
{
    auto te = TEManager::getInstance().getTEById(accessCtrl->header.teamEnvironmentID);
    // 新增更新当前任务状态前，如果是意图任务，查看所有的子任务，如果子任务是运行态的，stop掉，并且给gui发TaskStop报文
    auto children = taskNode->childrenNode;
    if (children.empty()) {
        LOG_INFO_EX("没有子任务");
        return;
    }
    for (const auto& child : children) {
        if (child->task->taskStatus != TaskStatus::RUNNING) {
            continue;
        }
        auto stopAccessCtrl = TEHelper::newTaskStop(te);
        if (stopAccessCtrl == nullptr) {
            LOG_INFO_EX("get stopAccessCtrl nullptr");
            continue;
        }
        stopAccessCtrl->setSender("SystemAgent");
        stopAccessCtrl->setReceiver("GUIAgent"); // 目前是这样的
        Stop(child, stopAccessCtrl);
    }
}

shared_ptr<AgentTask> WorkFlow::GetValidAgentTask()
{
    for (auto task : agentTasks) {
        if (task->isVisited == 0) {
            return task;
        }
    }
    return nullptr;
}

void ContextAddWithKMM(std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, shared_ptr<TaskNode> taskNode,
                       const std::string& userName)
{
    UserInfo userInfo = TEHelper::extractUserInfo(accessCtrl);
    auto qaEntity = taskNode->task->qaEntities;
    if (qaEntity.size() >= 2) {
        QAObject qaObject {
                .originalQA = GenerateQA(qaEntity[0].message, qaEntity[qaEntity.size() - 1].message),
                .rewriteQ = taskNode->task->data.params["RewriteQ"],
                .userId = userName,
        };
        UserInfo userInfo = TEHelper::extractUserInfo(accessCtrl);
        auto index = SYSAGENT::CommonFunc::GetStringHash(userInfo.userName, 10);
        auto qaObjectPtr = std::make_shared<QAObject>(qaObject);
        MemoryMgr::AddShortMemories("", "add", qaObjectPtr,
                                    [taskNode, userInfo, index](bool isSuccess, const std::string &response){
                                        if (!isSuccess) {
                                            return;
                                        }
                                        std::string uuid = "";
                                        Json::Value root;
                                        JsonParser::Parse(response, root);
                                        JsonParser::GetString(root, "id", uuid);
                                        QAQueueManager& instance = QAQueueManager::GetInstance();
                                        auto data = std::make_tuple(taskNode->task->qaEntities, userInfo.userName,
                                                                    taskNode->task->data.params["RewriteQ"], uuid, taskNode->task->taskID);
                                        instance.pushToQueue(index, data);
                                    });
    }
}

void ContextAddWithoutKMM(std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, shared_ptr<TaskNode> taskNode,
                          const std::string& userName)
{
    auto queryList = taskNode->task->queryEntities;
    auto answerList = taskNode->task->answerEntities;
    CHECK_TRUE_LOG_ERROR_RETURN_VOID(queryList.empty(), "insert qaEntity is empty");

    Json::Value data;
    uint32_t answerIndex = 0;

    for (uint32_t i = 0; i < queryList.size(); i++) {
        Json::Value item;
        item["query"] = queryList[i].message;
        item["custom_summary"] = "";
        item["re_write_query"] = taskNode->task->data.params["rewriteQ"];
        item["date"] = queryList[i].date;

        // 处理对应的answer
        std::string answerContent;
        while (answerIndex < answerList.size() && (i < queryList.size() - 1 ? answerList[answerIndex].index < queryList[i + 1].index : true)) {
            std::string message = answerList[answerIndex].message;
            size_t pos = message.find("通通正在进行联网搜索。");
            if (pos != std::string::npos) {
                message.erase(pos, std::string("通通正在进行联网搜索。").length());
            }
            answerContent += message + "\n";
            answerIndex++;
        }
        item["answer"] = answerContent;
        data.append(item);
    }
    std::string taskId = taskNode->task->taskID;
    taskId = taskId.substr(0, taskId.find('_'));
    std::string prefix = "======= Add Context =======\n";
    CommonFunc::AppendDebugInfoToFile(userName, taskId, prefix + JsonParser::ToRawString(data));
    ContextManager::AddContext(userName, "", "", accessCtrl->extractSender(), "", data, [taskId](bool isSuccess, std::vector<std::string>){
        LOG_INFO_EX("%s Task add context result: %d", taskId.c_str(), isSuccess);
    });
}

void ContextAddPolicy(std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, shared_ptr<TaskNode> taskNode, const std::string& userName)
{
    auto data = ConfigManager::getInstance().getEnvInfo("USE_LATEST_CONTEXT", "TRUE");
    if (data == "TRUE") {
        ContextAddWithoutKMM(accessCtrl, taskNode, userName);
    } else {
        ContextAddWithKMM(accessCtrl, taskNode, userName);
    }
}

const std::unordered_map<std::string, std::string> agentTypeMap = {
    {"GeneralShoppingAgent", "AnyShoppingAgent"},
    {"TravelBookingAgent", "LifeAssistantAgent"},
    {"InstantRetailAgent", "FoodieAgent"},
};

// 正常时的取值范围 TaskStart，TaskCompleted，UserClarificationCompleted
void WorkFlow::ChangeStatus(std::string taskID, shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, bool isRefuseAccessCtrl)
{
    if (workFlowStatus == WorkFlowStatus::FINISHED_BY_USER || workFlowStatus == WorkFlowStatus::FINISHED) {
        LOG_INFO_EX("cannot ChangeStatus, because the workflow have finished");
        return;
    }

    // 判断是否是状态上报报文，不是就结束
    if (accessCtrl->eventType != SYSAGENT::EventType::TASKSTATUSMANAGEMENT || accessCtrl->taskStatusManagement.data.messageType != "TaskStatusReport") {
        LOG_INFO_EX("the accessCtrl isnot TASKSTATUSMANAGEMENT or messageType isnot TaskStatusReport");
        return;
    }
    // 取出来任务状态报文的 taskStatusReport 字段
    std::string TaskStatusReport = accessCtrl->taskStatusManagement.data.taskStatusReport;

    std::cout << "[WorkFlow::ChangeStatus] use GetTaskNodeByTaskID" << std::endl;
    shared_ptr<TaskNode> taskNode = GetTaskNodeByTaskID(taskID);
    if (taskNode == nullptr) {
        LOG_INFO_EX("cannot find taskNode，, taskID : %s", taskID.c_str());
        return;
    }

    TaskStatus newStatus = taskNode->task->getStatus(); // 默认保持当前状态
    if (TaskStatusReport == "TaskCompleted") {
        ContextAddPolicy(accessCtrl, taskNode, userName);
        newStatus = TaskStatus::FINISHED;
    } else {
        LOG_INFO("The TaskStatusReport is not handled: %s", TaskStatusReport.c_str());
        return;
    }
    // 更新任务状态
    taskNode->task->setStatus(newStatus);
    LOG_INFO_EX("[ChangeStatus] Updated task status for task: %s to %d", taskID.c_str(), static_cast<int>(newStatus));

    // 如果不是意图任务，不需要找下一个执行
    std::vector<std::string> indexArray = CMFrm::Utils::SplitStr(taskID, "_");
    if (indexArray.size() != 2) {
        return;
    }
    StopSubTasks(taskNode, accessCtrl); // 意图任务，查看所有的子任务，如果子任务是运行态的，stop掉
    // 是意图任务，找到下一个，如果下一个任务存在，就执行
    size_t index = std::stoul(indexArray[1]) + 1;
    LOG_INFO_EX("Compare index and taskNodes.size(): %d and %d", static_cast<int>(index), static_cast<int>(taskNodes.size()));
    if (index < taskNodes.size()) {
        LOG_INFO_EX("multi task accessctrl header deviceCategory is:%s, inner deviceCategory is:%s",
                    accessCtrl->header.deviceCategory.c_str(), accessCtrl->taskStatusManagement.header.deviceCategory.c_str());
        std::string nextTaskID = indexArray[0] + "_" + std::to_string(index);
        // 直接拿到下一个TaskNode
        shared_ptr<TaskNode> nextTaskNode = taskNodes[index]; // uuid_1, uuid_2
        // 也是第一次执行，在exec里面统一
        shared_ptr<SYSAGENT::AccessCtrl> nextAccessCtrl = CopyTaskAssignment(accessCtrl, nextTaskNode, nextTaskID);
        std::thread thread([=] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            nextTaskNode->task->exec(nextAccessCtrl);
        });
        thread.detach();
        return;
    }
    // 判断 agentTask 是否还有未进行识别的任务
    auto agentTask = GetValidAgentTask();
    if (agentTask) {
        InferTask(agentTask, accessCtrl, index, indexArray);
        return;
    }
    LOG_INFO_EX("没有下一个任务了, workFlowTask结束");
    workFlowStatus = WorkFlowStatus::FINISHED;

    // 是拒答报文，直接返回
    if (isRefuseAccessCtrl) {
        return;
    }
    // 只是用teiD获取，获取不到就返回
    TEManager& teManager = TEManager::getInstance();
    std::string teamEnvId = accessCtrl->header.teamEnvironmentID;
    if (teamEnvId.empty()) {
        LOG_INFO_EX("[WorkFlow::ChangeStatus] teamEnvironmentID is empty");
        return;
    }
    std::shared_ptr<TeamEnvironment> teamEnvironment = teManager.getTEById(teamEnvId);
    if (teamEnvironment == nullptr) {
        LOG_INFO_EX("cannot find TE by id");
        return;
    }
    // 说明是最后一个任务，给前端APP返回结果报文，taskID变成workFlowID
    accessCtrl->taskStatusManagement.header.taskID = workFlowID;
    accessCtrl->taskStatusManagement.header.receiver = sender;
    accessCtrl->taskStatusManagement.header.clientExField = clientExField;
    QueueManager::getInstance().postWriteQueue(accessCtrl, teamEnvironment->session);
    // 调用方判断workFlowStatus来清理TE的workflow
}

std::shared_ptr<SYSAGENT::AccessCtrl> WorkFlow::CopyTaskAssignment(std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl,
                                                                   std::shared_ptr<TaskNode> nextTaskNode,
                                                                   std::string nextTaskID)
{
    shared_ptr<SYSAGENT::AccessCtrl> nextAccessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    nextAccessCtrl->eventType = SYSAGENT::EventType::TASKASSIGNMENT;
    nextAccessCtrl->taskAssignment.data.messageType = "TaskAssignment";
    nextAccessCtrl->taskAssignment.data.userInputText = nextTaskNode->task->requestContent;
    nextAccessCtrl->taskAssignment.header.sender = sender;
    nextAccessCtrl->taskAssignment.header.receiver = nextTaskNode->task->agentName;
    nextAccessCtrl->taskAssignment.header.messageType = "UserTaskManagement";
    nextAccessCtrl->taskAssignment.header.taskID = nextTaskID;
    nextAccessCtrl->taskAssignment.header.acmSessionId = accessCtrl->header.acmSessionId;
    nextAccessCtrl->taskAssignment.header.teamEnvironmentID = accessCtrl->header.teamEnvironmentID;
    nextAccessCtrl->taskAssignment.header.uuid = accessCtrl->header.uuid;
    nextAccessCtrl->taskAssignment.header.clientSessionId = accessCtrl->header.clientSessionId;
    nextAccessCtrl->taskAssignment.header.eventTimeStamp = AisfPlat::Utils::GetCurrentTimestampInCustomFormat();
    nextAccessCtrl->taskAssignment.header.sysAgentEntity = accessCtrl->header.sysAgentEntity;
    nextAccessCtrl->taskAssignment.header.userName = accessCtrl->header.userName;
    nextAccessCtrl->taskAssignment.header.deviceId = accessCtrl->header.deviceId;
    nextAccessCtrl->taskAssignment.header.deviceCategory = accessCtrl->header.deviceCategory;
    nextAccessCtrl->header = nextAccessCtrl->taskAssignment.header;
    nextTaskNode->task->addQ(nextAccessCtrl, taskNodes.size());
    AddOriginInputToKMM(nextAccessCtrl, shared_from_this());
    return nextAccessCtrl;
}

void WorkFlow::InferTask(shared_ptr<AgentTask> agentTask, shared_ptr<SYSAGENT::AccessCtrl> accessCtrl,
                         size_t index, std::vector<std::string> indexArray)
{
    std::string modelServiceName = ConfigManager::getInstance().getEnvInfo("INTENT_IDENTITY_MODEL_SERVICE_NAME",
                                                                           "llmlargeservice");
    mmsdk::MMReasonParam param = {
        .modelServiceName = modelServiceName, .modelName = "Qwen3-32B-Intent", .temperature = 0.001, .maxToken = 1024,
    };
    agentTask->isVisited = 1;
    auto prompt = PromptManager::GetInstance().GetPormptTmpl("manipulative_intent_identification");
    auto intention = mmsdk::MMUserIntention{.query = agentTask->intent,
                                            .modaltype = mmsdk::MMReasonModalType::MMLLM_MODAL_TYPE_TEXT};
    auto req = mmsdk::MMLLMRequest{.intentions = {intention}, .prompt = {prompt}};
    LLMomponent->RequestAsync(param, req, [self = shared_from_this(), accessCtrl, indexArray,
                                           index](uint32_t errorCode, const std::string& response) -> uint32_t {
        std::string contextLog = "======= ExecuteNode result remain =======\n" + response;
        UserInfo userInfo = TEHelper::extractUserInfo(accessCtrl);
        CommonFunc::AppendDebugInfoToFile(userInfo.userName, self->workFlowID, contextLog);
        LOG_INFO_EX("Sub Intent is:%s", response.c_str());
        auto result = trimThinkFlag(response);
        if (errorCode != mmsdk::MMErrorCode::SUCCESS) {
            LOG_INFO_EX("LLMRequestForSubIntent error, errorcode:%d", errorCode);
        }
        std::string trimResponse = CommonFunc::ExtractJsonString(response);
        Json::Value root;
        JsonParser::Parse(trimResponse, root);
        for (const auto& item : root) {
            if (JsonParser::IsMember(item, "assistant_name") && JsonParser::IsMember(item, "context")) {
                std::string agentName;
                std::string requestContent;
                JsonParser::GetString(item, "assistant_name", agentName);
                JsonParser::GetString(item, "context", requestContent);
                std::string taskID = self->workFlowID + "_" + std::to_string(self->taskNodes.size());
                Data data;
                std::string convertAgentName = agentTypeMap.count(agentName) ? agentTypeMap.at(agentName) : "GUIAgent";
                auto agent = Agent::getAgentInstance(convertAgentName);
                LOG_INFO_EX("Agent task add taskID:%s, agentName:%s", taskID.c_str(), convertAgentName.c_str());
                auto task = std::make_shared<ATATask>(taskID, agent, convertAgentName, requestContent, data);
                if (convertAgentName == "GUIAgent") {
                    task->isGUIGeneralization = true;
                }
                self->taskNodes.push_back(std::make_shared<TaskNode>(task));
            }
        }
        std::string nextTaskID = indexArray[0] + "_" + std::to_string(index);
        shared_ptr<TaskNode> nextTaskNode = self->taskNodes[index];
        shared_ptr<AccessCtrl> nextAccessCtrl = self->CopyTaskAssignment(accessCtrl, nextTaskNode, nextTaskID);
        nextTaskNode->task->exec(nextAccessCtrl);
        return 0;
    });
}


void WorkFlow::Stop(const shared_ptr<TaskNode>& taskNode, const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl)
{
    taskNode->task->stop(accessCtrl);
    // 递归停止子任务
    for (auto& childNode : taskNode->childrenNode) {
        Stop(childNode, accessCtrl);
    }
}

// stop the whole workflow
void WorkFlow::StopTask(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    // 判断workflow 状态，停了就不能执行
    if (workFlowStatus == WorkFlowStatus::FINISHED_BY_USER || workFlowStatus == WorkFlowStatus::FINISHED) {
        LOG_INFO_EX("cannot StopTask, because the workflow have finished");
        return;
    }
    // update the status of workflow with FinishByUser
    this->workFlowStatus = WorkFlowStatus::FINISHED_BY_USER;
    // find all the TaskStatus::RUNNING tasks in the workflow
    // 遍历当前 taskNodes 的所有元素，递归stopTask
    LOG_INFO("Still have %d tasks", taskNodes.size());
    for (uint32_t i = 0; i < taskNodes.size(); ++i) {
        shared_ptr<TaskNode> taskNode = taskNodes[i];
        LOG_INFO("TaskNode taskID %s", taskNode->task->taskID.c_str());
        Stop(taskNode, accessCtrl);
    }
}

void WorkFlow::ChangeReceiver(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    // 获取正确的header引用
    MsgAisfHeader* header = nullptr;
    switch (accessCtrl->eventType) {
        case SYSAGENT::EventType::TASKASSIGNMENT:
            header = &accessCtrl->taskAssignment.header;
            break;
        case SYSAGENT::EventType::TASKSTOP:
            header = &accessCtrl->taskStop.header;
            break;
        case SYSAGENT::EventType::USERINPUT:
            header = &accessCtrl->userInput.header;
            break;
        case SYSAGENT::EventType::USEROUTPUT:
            header = &accessCtrl->userOutput.header;
            break;
        case SYSAGENT::EventType::DATAEXCHANGE:
            header = &accessCtrl->dataExchange.header;
            break;
        case SYSAGENT::EventType::TASKSTATUSMANAGEMENT:
            header = &accessCtrl->taskStatusManagement.header;
            break;
        case SYSAGENT::EventType::LOGINOUT:
            header = &accessCtrl->loginOut.header;
            break;
        default:
            break;
    }
    if (header == nullptr) {
        return;
    }
    // 修改Header字段receiver为 workFlow的sender
    // 修改Header字段clientExField为 workFlow的clientExField
    LOG_INFO_EX("[WorkFlow::ChangeReceiver] change receiver : %s to sender : %s", header->receiver.c_str(), sender.c_str());
    LOG_INFO_EX("[WorkFlow::ChangeReceiver] change clientExField : %s to clientExField : %s", header->clientExField.c_str(), clientExField.c_str());
    header->receiver = sender;
    header->clientExField = clientExField;
    accessCtrl->header.receiver = sender;
    accessCtrl->header.clientExField = clientExField;
}

/**
 * 格式化时间字符串
 * @param input
 * @return
 */
std::string WorkFlow::FormatTimeString(const std::string& input)
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

void WorkFlow::AddOriginInputToKMM(const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl, shared_ptr<WorkFlow> workFlow)
{
    auto te = TEManager::getInstance().getTEById(accessCtrl->header.teamEnvironmentID);
    if (te == nullptr) {
        LOG_ERR_EX("te is null");
        return;
    }
    std::string workFlowID = "";
    if (te->getWorkFlow()) {
        workFlowID = te->getWorkFlow()->workFlowID;
    }
    Memory memory {
            .userId = te->getUserInfo().userName,
            .sessionId = workFlowID, ///te->getUserInfo().clientSessionId,
            .role = "user",
            .date = FormatTimeString(AisfPlat::Utils::GetCurrentTimestampInCustomFormat())
    };
    std::string sender = accessCtrl->extractSender();
    std::string receiver = accessCtrl->extractReceiver();
    if (sender == "GUIAgent" || receiver == "GUIAgent" || (sender == "QAAgent" && receiver == "ThirdProxyAgent")) {
        return;
    }
    // 可以直接拿eventType判断
    // 三类消息： 前端任务创建user、前端澄清结果user、后端的澄清消息system，封装方法构造结构体放入队列，user
    LOG_INFO_EX("receiver is : [%s]", accessCtrl->header.receiver.c_str());
    if (accessCtrl->eventType == SYSAGENT::EventType::TASKASSIGNMENT) {
        LOG_INFO_EX("batchMemory, userInputText [%s]", accessCtrl->taskAssignment.data.userInputText.c_str());
        memory.content = accessCtrl->taskAssignment.data.userInputText;
        QueueManager::getInstance().postKmmMsgReadQueue(memory);
        return;
    }
    if (accessCtrl->eventType == SYSAGENT::EventType::USERINPUT) {
        memory.content = accessCtrl->userInput.data.userInputText;
        if (!memory.content.empty()) { // 如果是走userInputObject，比如截图，那么就会为空
            LOG_INFO_EX("batchMemory, userInputText [%s]", memory.content.c_str());
            QueueManager::getInstance().postKmmMsgReadQueue(memory);
        }
        return;
    }
    if (accessCtrl->eventType == SYSAGENT::EventType::USEROUTPUT) { // 2 澄清报文
        if (accessCtrl->userOutput.data.userOutPutFlag == 2) {
            memory.role = "system";
            LOG_INFO_EX("batchMemory, userOutputText [%s]", accessCtrl->userOutput.data.userOutputText.c_str());
            memory.content = accessCtrl->userOutput.data.userOutputText;
            if (!memory.content.empty() && memory.content.find("根据查询到的历史记忆") != std::string::npos
                && sender == "TravelAgent") {
                return;
            }
            QueueManager::getInstance().postKmmMsgReadQueue(memory);
        } else if (accessCtrl->userOutput.data.userOutPutFlag == 0 && accessCtrl->extractSender() != "GUIAgent") {
            CHECK_NULL_LOG_ERROR_RETURN_VOID(workFlow, "add origin conversion, workflow is null");
            memory.role = "system";
            if (accessCtrl->userOutput.data.streamStatus == "Running") {
                LOG_INFO_EX("add tips userOutputText [%s]", accessCtrl->userOutput.data.userOutputText.c_str());
                workFlow->streamOutput += accessCtrl->userOutput.data.userOutputText;
            } else if (accessCtrl->userOutput.data.streamStatus == "Completed") {
                LOG_INFO_EX("insert tips userOutputText [%s]", accessCtrl->userOutput.data.userOutputText.c_str());
                workFlow->streamOutput += accessCtrl->userOutput.data.userOutputText;
                memory.content = workFlow->streamOutput;
                workFlow->streamOutput.clear();
                QueueManager::getInstance().postKmmMsgReadQueue(memory);
            }
        }
        return;
    }
}
// workflowID_0
void WorkFlow::ExecTask(string taskID, shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    // 判断workflow 状态，停了就不能执行
    if (workFlowStatus == WorkFlowStatus::FINISHED_BY_USER || workFlowStatus == WorkFlowStatus::FINISHED) {
        LOG_INFO_EX("cannot ExecTask, because the workflow have finished");
        return;
    }
    // find the task
    std::cout << "[WorkFlow::ExecTask] use GetTaskNodeByTaskID" << std::endl;
    shared_ptr<TaskNode> taskNode = GetTaskNodeByTaskID(taskID);
    if (taskNode == nullptr) {
        LOG_INFO_EX("cannot find the taskNode");
        return;
    }
    nowSender = accessCtrl->header.sender;
    nowTaskId = taskID;
    LOG_INFO_EX("[WorkFlow::ExecTask] accessCtrl->header.receiver : %s, accessCtrl->eventType : %d",
                accessCtrl->header.receiver.c_str(), static_cast<int>(accessCtrl->eventType));
    LOG_INFO_EX("accessCtrl head clientExField is:%s, clientExField is:%s",
                accessCtrl->header.clientExField.c_str(), clientExField.c_str());
    if (accessCtrl->header.clientExField != clientExField && !clientExField.empty() &&
        !accessCtrl->header.clientExField.empty()) {
        clientExField = accessCtrl->header.clientExField;
    }
    // 如果接收到各个业务agent的报文中接收方的是APP，需要将接收方修改为workFlow记录的发送方
    if (accessCtrl->header.receiver == "APP" && accessCtrl->eventType != SYSAGENT::EventType::DATAEXCHANGE) {
        LOG_INFO_EX("[WorkFlow::ExecTask] ChangeReceiver");
        ChangeReceiver(accessCtrl);
    }
    taskNode->task->addQ(accessCtrl, taskNodes.size());
    taskNode->task->addA(accessCtrl);
    // 把原始输入存到KMM
    AddOriginInputToKMM(accessCtrl, shared_from_this());
    CHECK_NULL_LOG_ERROR_RETURN_VOID(taskNode->task, "taskNode's task is nullptr");
    LOG_INFO_EX("[WorkFlow::ExecTask] before task->exec taskNode=%p task=%p taskID=%s taskStatus=%d",
                taskNode.get(), taskNode->task.get(), taskNode->task->taskID.c_str(),
                static_cast<int>(taskNode->task->taskStatus));
    taskNode->task->exec(accessCtrl);
    LOG_INFO_EX("[WorkFlow::ExecTask] after task->exec taskID=%s", taskID.c_str());
    UpdateWorkFlowLiveTime();
    // 如果当前是终止任务，清空后续所有的task
    if (accessCtrl->eventType == SYSAGENT::EventType::TASKSTOP) {
        auto currentTaskID = taskNode->task->taskID;
        for (auto it = taskNodes.begin(); it != taskNodes.end();) {
            if (it->get() && it->get()->task && it->get()->task->taskID > currentTaskID) {
                it = taskNodes.erase(it);
            } else {
                ++ it;
            }
        }
        for (auto& task : agentTasks) {
            task->isVisited = 1;
        }
    }
}

shared_ptr<TaskNode> WorkFlow::GetTaskNodeByTaskID(std::string taskID)
{
    if (taskID.empty()) {
        LOG_ERR_EX("[GetTaskNodeByTaskID] taskID is Empty");
        return nullptr;
    }
    std::cout << "[WorkFlow::GetTaskNodeByTaskID] taskid : " << taskID << std::endl;
    std::vector<std::string> indexArray = CMFrm::Utils::SplitStr(taskID, "_");
    if (indexArray.size() < 2) {
        // 如果任务ID格式不正确，返回空指针
        LOG_ERR_EX("taskID is wrong format : %s", taskID.c_str());
        return nullptr;
    }
    if (taskNodes.size() <= static_cast<size_t>(std::stoi(indexArray[1]))) {
        return nullptr;
    }
    shared_ptr<TaskNode> taskNode = taskNodes[std::stoi(indexArray[1])]; // uuid_1, uuid_2
    for (uint32_t i = 2; i < indexArray.size(); ++i) {
        if (taskNode->childrenNode.size() <= static_cast<size_t>(std::stoi(indexArray[i]))) {
            // 如果子节点不存在，返回空指针
            return nullptr;
        }
        taskNode = taskNode->childrenNode[std::stoi(indexArray[i])];
    }
    return taskNode;
}

void WorkFlow::SetAgentFlowDialogueId(const string& dialogueId)
{
    std::shared_lock<std::shared_mutex> lk(workflowMutex);
    this->agentflowDialogueId = dialogueId;
}

std::string WorkFlow::GetAgentFlowDialogueId()
{
    std::unique_lock<std::shared_mutex> lk(workflowMutex);
    return agentflowDialogueId;
}

void WorkFlow::SetAgentFlowSessionId(const string& sessionId)
{
    std::shared_lock<std::shared_mutex> lk(workflowMutex);
    this->agentflowSessionId = sessionId;
}

std::string WorkFlow::GetAgentFlowSessionId()
{
    std::unique_lock<std::shared_mutex> lk(workflowMutex);
    return agentflowSessionId;
}

// 生成8位随机数字的taskId
std::string WorkFlow::GenerateRandomWorkFlowID()
{
    std::string result;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, 9);

    for (int i = 0; i < 8; ++i) {
        result += std::to_string(dist(gen));
    }
    return result;
}

void WorkFlow::UpdateWorkFlowLiveTime()
{
    lastUpdateTime = std::chrono::system_clock::now();
}

bool WorkFlow::CheckWorkFlowTimeOut(uint32_t timeoutSeconds)
{
    std::shared_lock<std::shared_mutex> lk(workflowMutex);
    auto now = std::chrono::system_clock::now();
    auto duration = now - lastUpdateTime;
    auto elapsedSeconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    return elapsedSeconds > timeoutSeconds;
}

bool WorkFlow::IsWorkFlowFinished()
{
    std::shared_lock<std::shared_mutex> lk(workflowMutex);
    return workFlowStatus == WorkFlowStatus::FINISHED;
}

std::string WorkFlow::GetNowTaskId()
{
    std::shared_lock<std::shared_mutex> lk(workflowMutex);
    return nowTaskId;
}

void WorkFlow::SetWorkflowStatusFinished()
{
    std::unique_lock<std::shared_mutex> lk(workflowMutex);
    workFlowStatus = WorkFlowStatus::FINISHED;
}

std::string WorkFlow::GetNowSender()
{
    std::shared_lock<std::shared_mutex> lk(workflowMutex);
    return nowSender;
}

std::string WorkFlow::GetTopSender()
{
    std::shared_lock<std::shared_mutex> lk(workflowMutex);
    return sender;
}

std::string WorkFlow::GetClientExField()
{
    std::shared_lock<std::shared_mutex> lk(workflowMutex);
    return clientExField;
}

MsgPayload WorkFlow::GetRootPayLoad()
{
    std::shared_lock<std::shared_mutex> lk(workflowMutex);
    return rootPayLoad;
}

std::string WorkFlow::GetWorkflowID()
{
    std::shared_lock<std::shared_mutex> lk(workflowMutex);
    return workFlowID;
}

void WorkFlow::SetUserName(const std::string& userName)
{
    std::unique_lock<std::shared_mutex> lk(workflowMutex);
    this->userName = userName;
}

std::shared_ptr<TaskNode> WorkFlow::GetTopTaskNode()
{
    std::shared_lock<std::shared_mutex> lk(workflowMutex);
    if (taskNodes.empty()) {
        return nullptr;
    }
    return taskNodes[0];
}

bool WorkFlow::HasTaskNotFinished()
{
    std::shared_lock<std::shared_mutex> lk(workflowMutex);
    if (this->taskNodes.empty() && !this->agentTasks.empty()) {
        return true;
    }
    return false;
}

std::string WorkFlow::GenNewTaskID()
{
    std::shared_lock<std::shared_mutex> lk(workflowMutex);
    return this->workFlowID + "_" + std::to_string(this->taskNodes.size());
}

void WorkFlow::AddNewTask(const std::shared_ptr<TaskNode>& taskNode)
{
    std::unique_lock<std::shared_mutex> lk(workflowMutex);
    taskNodes.push_back(taskNode);
}

void WorkFlow::SetAgentTasks(std::vector<shared_ptr<AgentTask>> agentTasks)
{
    std::unique_lock<std::shared_mutex> lk(workflowMutex);
    this->agentTasks = std::move(agentTasks);
}

bool WorkFlow::GetIntentForTopTask(std::string& intent)
{
    std::unique_lock<std::shared_mutex> lk(workflowMutex);
    if (agentTasks.empty()) {
        return false;
    }
    agentTasks[0]->isVisited = 1;
    intent = agentTasks[0]->intent;
    return true;
}

shared_ptr<TaskNode> WorkFlow::FindTaskNodeByTaskID(std::string taskID)
{
    std::shared_lock<std::shared_mutex> lk(workflowMutex);
    return GetTaskNodeByTaskID(std::move(taskID));
}

shared_ptr<TaskNode> WorkFlow::FindTaskNodeByAgentName(const std::string& agentName)
{
    std::shared_lock<std::shared_mutex> lk(workflowMutex);
    for (auto& node : taskNodes) {
        if (node->task && node->task->agentName == agentName) {
            return node;
        }
    }
    return nullptr;
}
