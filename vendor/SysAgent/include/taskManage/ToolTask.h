//
// Created by c00888021 on 2025/10/25.
//

#ifndef SYSAGENT_TOOLTASK_H
#define SYSAGENT_TOOLTASK_H
#include "Task.h"
#include "map"
#include "config/Config.h"
#include "model/ReactLLMTask.h"
#include "taskManage/TeamEnvironment.h"
#include "memory/SharedMemory.h"
#define CLARIFY_STATUS "澄清状态"
#define CLARIFY_PARAMS "提取参数列表"
#define CLARIFY_QUESTION "澄清问题"
#define CLARIFY_STATUS_FINISHED "完成"
#define CLARIFY_STATUS_ING "澄清中"
#define CLARIFY_STATUS_INVALID "澄清非法"

struct ClarifyResult {
    std::string clarifyStatus{""};
    std::map<std::string, std::string> paramList;
    std::string question;
};

struct ToolTaskCallback {
    uint32_t code;
    uint32_t retCode;
    std::string sendType;
    std::string msg;
    Json::Value data;
};

enum ToolRetCode {
    THREE = 200003,
    FOUR = 200004,
    FIVE = 200005
};

ClarifyResult ParseClarifyResult(std::string input);
std::string AddParamsToMsgCallBack(std::string msgCallBack, std::map<std::string, std::string> paramMap);
std::string ReplacePlaceholders(const std::string& templateStr, const std::string& placeholder,
                                const std::string& value);

class ToolTask : public Task, public std::enable_shared_from_this<ToolTask> {
public:
    ToolTask(std::string& taskID, std::string& agentName, std::string& requestContent, std::string& workFlowID);
    void exec(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl) override;
    void stop(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl) override;
    void StartApi(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl);
    void RegisterToolTaskCallback(std::shared_ptr<ReactLLMTask> task, shared_ptr<SYSAGENT::AccessCtrl> accessCtrl,
                                  const std::string& teId);
    void CreateToolTask(const std::string& userInput, std::shared_ptr<TeamEnvironment>& te,
                        const mmsdk::MMPromptTemplate& prompt, const std::string& teId,
                        shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl);
    bool SendTaskStatusReport(int statusCode, std::string statusReport, std::string teId);
    void FindTool(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl);
    void CompleteCommonParams(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl);
    void CompleteModelParams(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, ClarifyResult& result);
    void ExecMcpTool(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl);
    void ExecLocalTool(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl);
    static void SendToolUserOutputMsg(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, const std::string& callback);
    static void SendSpecifiedUserOutputMsg(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl,
                                           const std::string& content, const std::string& outputObject,
                                           const std::string& status, int flag, const std::string& receiver);
    ToolMessage ParseToolInterface(const std::string& query);
    void GenerateContext(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, std::string role, std::string content);
private:
    std::string workFlowID;
    bool isClarify{false};
    ToolMessage toolMessage;
    std::string apiDescriptions;
    bool needExtractParams{false};
    std::shared_ptr<std::vector<ShortMemory>> memories;
    std::string toolName;
    std::string toolType;
};

#endif // SYSAGENT_TOOLTASK_H
