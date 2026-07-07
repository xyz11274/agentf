//
// Created by z00913804 on 25-5-19.
//

#ifndef SYSAGENT_TASKMANAGE_H
#define SYSAGENT_TASKMANAGE_H

#include <algorithm>
#include <string>
#include <utility>

#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "taskManage/TaskEntity.h"
#include "taskManage/TaskNode.h"
#include "AgentManager.h"
#include <set>

#define SerialLModel "SerialLModel"
#define ParallelRAG "ParallelRAG"
#define ParallelSModel "ParallelSModel"
#define ParallelLModel "ParallelLModel"
#define UnifyFastIntentModel "UnifyFastIntentModel"
#define UnifySlowIntentModel "UnifySlowIntentModel"

class IntentTask {
public:
    static std::set<std::string> thirdAgent;
    std::string agentName;
    std::string requestContent;
    std::string APP;
    std::string intentPath;
    uint32_t step{0};
    std::unordered_set<uint32_t> dependsOn;
    std::string condition;
    Data data;
    bool paramsMatch{false};
    IntentTask() = default;
    IntentTask(std::string agentname, std::string requestcontent, std::string app)
        : agentName(std::move(agentname)), requestContent(std::move(requestcontent)), APP(std::move(app)) {};
    IntentTask(std::string agentname, std::string requestcontent, std::string app, std::string intentPath, Data data)
        : agentName(std::move(agentname)), requestContent(std::move(requestcontent)),
        APP(std::move(app)), intentPath(std::move(intentPath)), data(std::move(data)) {};
    IntentTask(std::string agentname, std::string requestcontent, std::string app, std::string intentPath, Data data,
               bool paramsMatch)
        : agentName(std::move(agentname)), requestContent(std::move(requestcontent)),
        APP(std::move(app)), intentPath(std::move(intentPath)), data(std::move(data)), paramsMatch(paramsMatch) {}
    static std::vector<AgentData> GetAgentData(std::vector<IntentTask> intents)
    {
        std::vector<AgentData> agentDatas;
        auto agentInfos = AgentManager::getInstance().getAgentMap();
        for (auto& item : agentInfos) {
            auto data = item.second;
            AgentData agentData(data.type, data.name, data.serviceName, data.baseUrl, data.port);
            agentDatas.push_back(agentData);
        }
        return agentDatas;
    }
};

enum class WorkFlowStatus {
    PENDING,
    RUNNING,
    FINISHED,
    FAILED,
    FINISHED_BY_USER,
};

class WorkFlow : public std::enable_shared_from_this<WorkFlow> {
public:
    WorkFlow(std::string workflowID = "");

    bool operator==(const WorkFlow& other) const {
        return this->workFlowID == other.workFlowID;
    }

    bool operator!=(const WorkFlow& other) const {
        return this->workFlowID != other.workFlowID;
    }

    /**
     * 设置dialogueId
     * @param dialogueId
     */
    void SetAgentFlowDialogueId(const std::string& dialogueId);

    /**
     * 获取dialogueId
     * @return
     */
    std::string GetAgentFlowDialogueId();

    /**
     * 设置sessionId
     * @param sessionId
     */
    void SetAgentFlowSessionId(const std::string& sessionId);

    /**
     * 获取sessionId
     * @return
     */
    std::string GetAgentFlowSessionId();

    /**
     * 检查当前任务是否超时
     * @return 是否超过指定时间
     */
    bool CheckWorkFlowTimeOut(uint32_t timeoutSeconds);

    /**
     * 创建workflow任务
     * @param intentTasks
     * @param accessCtrl
     * @param agentDatas
     * @return
     */
    bool CreateWorkFlowTask(std::vector<IntentTask>& intentTasks, const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl,
                            std::vector<AgentData>& agentDatas);

    /**
     * 创建子任务
     * @param taskEntity
     * @param accessCtrl
     * @return
     */
    std::string CreateSubTask(TaskEntity taskEntity, shared_ptr<SYSAGENT::AccessCtrl> accessCtrl);

    /**
     * 改变workflow执行状态
     * @param taskID
     * @param accessCtrl
     * @param isRefuseAccessCtrl
     */
    void ChangeStatus(std::string taskID, shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, bool isRefuseAccessCtrl = false);

    /**
     * 终止任务执行
     * @param accessCtrl
     */
    void StopTask(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl);

    /**
     * 执行任务
     * @param taskID
     * @param accessCtrl
     */
    void ExecTask(string taskID, shared_ptr<SYSAGENT::AccessCtrl> accessCtrl);

    /**
     * 生成8位随机数字的 workflow id
     * @return
     */
    static std::string GenerateRandomWorkFlowID();

    /**
     * workflow是否执行结束
     * @return
     */
    bool IsWorkFlowFinished();

    /**
     * 设置workflow执行状态为结束
     */
    void SetWorkflowStatusFinished();

    /**
     * 获取当前taskId
     * @return
     */
    std::string GetNowTaskId();

    /**
     * 获取当前Sender
     * @return
     */
    std::string GetNowSender();

    /**
     * 获取顶级sender
     * @return
     */
    std::string GetTopSender();

    /**
     * 获取用户自定义字段
     * @return
     */
    std::string GetClientExField();

    /**
     * 获取根节点paylaod字段
     * @return
     */
    MsgPayload GetRootPayLoad();

    /**
     * 获取workflowID
     * @return
     */
    std::string GetWorkflowID();

    /**
     * 获取workflowID
     * @return
     */
    void SetUserName(const std::string& userName);

    /**
     * 获取index为0的任务
     * @return
     */
    std::shared_ptr<TaskNode> GetTopTaskNode();

    /**
     * 判断是否还有任务未执行完
     * @return
     */
    bool HasTaskNotFinished();

    /**
     * 为新任务生成新的taskID
     * @return
     */
    std::string GenNewTaskID();

    /**
     * 添加新任务
     * @param taskNode
     */
    void AddNewTask(const std::shared_ptr<TaskNode>& taskNode);

    /**
     * 设置AgentTasks
     * @param agentTasks
     */
    void SetAgentTasks(std::vector<shared_ptr<AgentTask>> agentTasks);

    /**
     * 获取top task的intent
     * @return
     */
    bool GetIntentForTopTask(std::string& intent);

    /**
     * 通过taskId获取taskNode
     * @param taskID
     * @return
     */
    shared_ptr<TaskNode> FindTaskNodeByTaskID(std::string taskID);
    shared_ptr<TaskNode> FindTaskNodeByAgentName(const std::string& agentName);

private:
    std::vector<shared_ptr<TaskNode>> taskNodes;
    std::vector<shared_ptr<AgentTask>> agentTasks;

    std::string workFlowID;
    std::string userName;
    MsgPayload rootPayLoad;
    std::string clientExField;
    std::string sender;
    std::string streamOutput;
    std::string nowSender;
    std::string nowTaskId;
    WorkFlowStatus workFlowStatus;
    std::string agentflowSessionId;
    std::string agentflowDialogueId;

    std::shared_mutex workflowMutex;
    std::chrono::time_point<std::chrono::system_clock> lastUpdateTime;

    /**
     * 通过taskId获取taskNode
     * @param taskID
     * @return
     */
    shared_ptr<TaskNode> GetTaskNodeByTaskID(std::string taskID);

    /**
     * 更新workflow的存活时间
     */
    void UpdateWorkFlowLiveTime();

    /**
     * 初始化workflow
     *
     * @param intentTasks
     * @param accessCtrl
     * @param agentDatas
     * @return
     */
    bool InitWorkFlow(std::vector<IntentTask>& intentTasks, shared_ptr<SYSAGENT::AccessCtrl> accessCtrl,
        std::vector<AgentData>& agentDatas);

    /**
     * 帮助函数，格式话时间字符串
     * @param input
     * @return
     */
    std::string FormatTimeString(const std::string& input);

    /**
     * 获取未执行的agent任务
     * @return
     */
    shared_ptr<AgentTask> GetValidAgentTask();

    /**
     * 停止workflow
     * @param taskNode
     * @param accessCtrl
     */
    void Stop(const shared_ptr<TaskNode>& taskNode, const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl);

    /**
     * 终止子任务
     * @param taskNode
     * @param accessCtrl
     */
    void StopSubTasks(const shared_ptr<TaskNode>& taskNode, const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl);

    /**
     * 发送任务创建失败报文
     * @param accessCtrl
     * @param workFlowIDorParentTaskID
     * @param taskType
     */
    void SendTaskCreateFailedMessage(
        shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, string workFlowIDorParentTaskID, string taskType);

    /**
     * 改变消息接收方
     * @param accessCtrl
     */
    void ChangeReceiver(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl);

    /**
     * 帮助函数，拷贝TaskAssignment
     * @param accessCtrl
     * @param nextTaskNode
     * @param nextTaskID
     * @return
     */
    std::shared_ptr<SYSAGENT::AccessCtrl> CopyTaskAssignment(std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl,
        std::shared_ptr<TaskNode> nextTaskNode,
        std::string nextTaskID);

    /**
     * 将用户原始输入保存至KMM
     * @param accessCtrl
     * @param workFlow
     */
    void AddOriginInputToKMM(const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl, shared_ptr<WorkFlow> workFlow);

    /**
     * 推理任务
     * @param agentTask
     * @param accessCtrl
     * @param index
     * @param indexArray
     */
    void InferTask(shared_ptr<AgentTask> agentTask, shared_ptr<SYSAGENT::AccessCtrl> accessCtrl,
        size_t index, std::vector<std::string> indexArray);
};
#endif // SYSAGENT_TASKMANAGE_H
