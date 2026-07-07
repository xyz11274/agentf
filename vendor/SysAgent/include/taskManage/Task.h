//
// Created by z00913804 on 25-5-19.
//

#ifndef SYSAGENT_TASK_H
#define SYSAGENT_TASK_H

#include <iomanip>
#include <memory>
#include <string>

#include "mm_customedreact_llm_component.h"

#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "taskManage/Agent.h"
#include "memory/QaManager.h"

// Task状态枚举
enum class TaskStatus { PENDING, RUNNING, FINISHED, FAILED, FINISHED_BY_USER };

enum class TaskType { ATATask, ModelTask, RouteTask };
std::string getCleanResponse(std::string response);

class Data {
public:
    std::string toolName;
    std::string category;
    std::string intent;
    std::map<std::string, std::string> params;

    Data() {};
    Data(const std::string& category, const std::string& intent, const std::map<std::string, std::string>& params)
        : category(category),
          intent(intent),
          params(params)
    {
    }

    std::string serialize() const;
    static void deserialize(Data& data, const std::string& jsonStr);
    Json::Value parseJson();
};

struct FullData {
    int retCode;
    int code;
    std::string msg;
    Data data;
    std::string ToString() {
        Json::Value root;
        root["retCode"] = retCode;
        root["code"] = code;
        root["msg"] = msg;
        root["data"] = data.parseJson();
        return JsonParser::ToRawString(root);
    }
};

struct IntentInfo {
    bool isDeepThink = false;
    bool isWithMemory = false;
    bool isDeepResearch = false;
    bool isMemDebug = false;
};

IntentInfo parseIntentInfo(const std::string &userIntentStr);

class Task {
public:
    std::string taskType;
    std::string agentName;
    std::string requestContent;
    std::string taskID;
    std::string intentPath;
    TaskStatus taskStatus;
    Data data;
    vector<QAEntity> qaEntities;
    vector<QAEntity> queryEntities;
    vector<QAEntity> answerEntities;
    uint32_t qaIndex{0};

public:
    virtual ~Task() = default;
    virtual void exec(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl) = 0;
    void setTaskID(std::string taskId);
    std::string findTaskID();
    void setStatus(TaskStatus status);
    TaskStatus getStatus();
    virtual void stop(shared_ptr<SYSAGENT::AccessCtrl>) = 0;
    std::string getTaskType();
    void addQ(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, size_t number);
    void addA(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl);
    bool filterContext(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl);
};

class ATATask : public Task {
public:
    std::shared_ptr<Agent> agent;
    bool isGUIGeneralization{false};

public:
    ATATask(std::string& taskID, std::shared_ptr<Agent> agent, std::string& agentName, std::string& requestContent);
    ATATask(std::string& taskID, std::shared_ptr<Agent> agent, std::string& agentName, std::string& requestContent, const Data& data);
    void exec(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl) override;
    std::string buildMessageBody(SYSAGENT::AccessCtrl& accessCtrl) const;
    // Agent管理方法
    bool setAgent(std::shared_ptr<Agent> agent);
    bool clearAgent();
    std::shared_ptr<Agent> getAssignedAgent() const;
    void stop(shared_ptr<SYSAGENT::AccessCtrl>) override;
};

class RouteTask : public Task {
public:
    RouteTask(std::string& taskID, std::string& agentName, std::string& requestContent);
    void exec(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl) override;
    void stop(shared_ptr<SYSAGENT::AccessCtrl>) override;
};

class ModelTask : public Task, public std::enable_shared_from_this<ModelTask> {
public:
    ModelTask(std::string& taskID, std::string& agentName, std::string& requestContent);
    void exec(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl) override;
    void stop(shared_ptr<SYSAGENT::AccessCtrl>) override;

private:
};

std::string getLocation(const std::string& deviceId);

void createStreamTask(std::string sumModel, std::string toolModel,
                      std::string sumModelService, std::string toolModelService,
                      std::shared_ptr<mmsdk::MMLLMAsyncCallBack> streamCallback,
                      std::function<uint32_t(uint32_t errorCode, std::string& taskId)> callback);
void changeUserIntent(std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, std::string& taskId);

void sendStreamRequest(std::string taskid, std::string query, const IntentInfo &userIntent,
                       const std::string& deviceId);
void sendStreamRequestDispatch(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, std::string taskId,
                               const IntentInfo& userIntent);

void addUerChatMessage(const std::string& userid, const std::string& clientSessionId, const std::string& content);

inline std::string today_date()
{
    // 获取当前时间点
    auto now = std::chrono::system_clock::now();
    // 转换为time_t类型
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
    // 转换为tm结构体
    std::tm* now_tm = std::localtime(&now_time_t);

    // 星期名称数组
    static const std::string weekdays_zh[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};

    // 格式化日期和时间
    std::ostringstream oss;
    oss << std::put_time(now_tm, "%Y-%m-%d ") << weekdays_zh[now_tm->tm_wday];

    return oss.str();
}

class CommandTask : public Task {
public:
    CommandTask(string& taskID, string& agentName, string& requestContent, string& intentPath, Data& data1);
    CommandTask(string& taskID, string& agentName, string& requestContent, string& intentPath, Data& data1,
                bool paramsMatch);
    void exec(shared_ptr<SYSAGENT::AccessCtrl>) override;
    void stop(shared_ptr<SYSAGENT::AccessCtrl>) override;
    void sendUserOutputMsg(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl,
                           string content, string status, int flag);
    void sendNewTaskStatusReport(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl);
    void modelExtractParams(string modelName, string modelServiceName,
                            const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl,
                            const string& requestContent, string paramsJsonStr);
    void extractParamsByLLM(string modelName, string modelServiceName,
                            const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl,
                            const string& requestContent);
private:
    bool paramsMatch{false};
};

class AgentTask {
public:
    std::string type;
    std::string intent;
    uint32_t isVisited{0};
    AgentTask() {}
    AgentTask(std::string taskType, const std::string& taskIntent) : type(taskType), intent(taskIntent) {}
};

#endif // SYSAGENT_TASK_H
