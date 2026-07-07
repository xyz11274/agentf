#include "taskManage/ToolTask.h"
#include <future>
#include <memory>
#include <utility>

#include "framework_common/utils/env_util.h"

#include "taskManage/Task.h"
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
#define LOCAL "LOCAL"
#define MCP "MCP"
#include "model/ReactLLMTask.h"
#include "memory/SharedMemory.h"
#include "aisf_platform/string/string_utils.h"
#include "utils/tool.h"

#include "registerManage/ToolManager.h"
ToolTask::ToolTask(std::string& taskID, std::string& agentName, std::string& requestContent, std::string& workFlowID)
{
    this->taskID = taskID;
    this->agentName = agentName;
    this->requestContent = requestContent;
    this->workFlowID = workFlowID;
    this->taskStatus = TaskStatus::PENDING;
    memories = std::make_shared<std::vector<ShortMemory>>();
}

void ToolTask::exec(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    LOG_INFO_EX("ToolTask [%s] running", taskID.c_str());
    accessCtrl->taskAssignment.data.userInputText = requestContent;
    setStatus(TaskStatus::RUNNING);
    accessCtrl->setTaskID(taskID);

    // 获取te
    std::string teamEnvId = TEManager::getInstance().extractTeamEnvId(accessCtrl);
    auto te = TEManager::getInstance().getTEById(teamEnvId);

    if (!isClarify) {
        // 获取工具集合
        FindTool(accessCtrl);
        // 封装公共参数
        CompleteCommonParams(accessCtrl);
    }

    if (toolMessage.name.empty()) {
        TEHelper::sendUserOutputMsg(accessCtrl, "工具列表中未找到相应的工具", UserOutput_Completed, 0);
        SendTaskStatusReport(CMFrm::COM::HttpStatus::HTTP200, TaskStatusReport_TaskCompleted, teamEnvId);
        return;
    }

    if (!needExtractParams && toolMessage.type == LOCAL) {
        ExecLocalTool(accessCtrl);
        return;
    }

    if (!needExtractParams && toolMessage.type == MCP) {
        ExecMcpTool(accessCtrl);
        return;
    }

    if (needExtractParams) {
        // 执行前澄清操作
        isClarify = true;
        StartApi(accessCtrl);
    }
}

void ToolTask::stop(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
}

std::string AddParamsToMsgCallBack(std::string msgCallBack, std::map<std::string, std::string> paramMap)
{
    std::string jsonStr = msgCallBack;
    // 将字符串转换为输入流
    std::istringstream jsonStream(jsonStr);
    // 创建 JSON 对象
    Json::Value root;
    Json::CharReaderBuilder builder;
    JSONCPP_STRING errs;
    if (!Json::parseFromStream(builder, jsonStream, &root, &errs)) {
        LOG_INFO_EX("JSON 解析失败");
        std::cerr << "JSON 解析失败: " << errs << std::endl;
        return msgCallBack;
    }
    // 将 map 转换为 JSON 对象列表
    for (const auto& [key, value] : paramMap) {
        Json::Value param;
        param[key] = value;
        root["data"]["params"].append(param);
    }
    // 输出最终的 JSON 字符串
    Json::FastWriter writer;
    std::string result = writer.write(root);
    LOG_INFO_EX("result : %s", result.c_str());
    return result;
}

void ToolTask::StartApi(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    auto tmpPrompt = PromptManager::GetInstance().GetPormptTmpl("tool_agent_prompt");
    tmpPrompt.RenderPrompt({{"current_date", today_date()}, {"tool_api", apiDescriptions},
                            {"tool_description", toolMessage.description}});
    LOG_INFO_EX("prompt is : %s", tmpPrompt.GetContent().c_str());
    std::cout << toolMessage.ToString() << std::endl;
    std::cout << tmpPrompt.GetContent() << std::endl;
    // 获取te
    std::string teamEnvId = TEManager::getInstance().extractTeamEnvId(accessCtrl);
    auto te = TEManager::getInstance().getTEById(teamEnvId);
    CHECK_NULL_LOG_ERROR_RETURN_VOID(te, "te is null");
    if (accessCtrl->header.messageType == "UserTaskManagement") {
        if (accessCtrl->eventType == SYSAGENT::EventType::TASKSTOP) {
            auto statusReport = TEHelper::newTaskStatusReport(te->getTeamId(), 200, "TaskCompleted");
            CHECK_NULL_LOG_ERROR_RETURN_VOID(statusReport, "status report is null");
            statusReport->setTaskID(taskID);
            statusReport->setSender("SystemAgent");
            statusReport->setReceiver("APP");
            shared_ptr<WorkFlow> workFlow = te->getWorkFlow(taskID);
            CHECK_NULL_LOG_ERROR_RETURN_VOID(workFlow, "workFlow is null");
            workFlow->ChangeStatus(taskID, statusReport);
        } else if (accessCtrl->eventType == SYSAGENT::EventType::TASKASSIGNMENT) {
            CreateToolTask(requestContent, te, tmpPrompt, teamEnvId, accessCtrl);
        }
    } else if (accessCtrl->header.messageType == "DataExchange") {
        CHECK_NULL_LOG_ERROR_RETURN(te, void(), "GET te failed");
        auto toolTaskId = te->getToolTaskId();
        if (toolTaskId != "-1") {
            auto intention = MMUserIntention{.query = accessCtrl->extractInput(), .modaltype = MMReasonModalType::MMLLM_MODAL_TYPE_TEXT};
            auto req = mmsdk::MMLLMRequest{
                    .intentions = {intention},
                    .prompt = {tmpPrompt}
            };
            RegisterToolTaskCallback(te->getToolTask(), accessCtrl, teamEnvId);
            auto ret = reactLLMomponent->RequestAsync(toolTaskId, req, {});
            LOG_INFO_EX("toolTask ret is:%d", ret);
        } else {
            LOG_INFO_EX("toolTaskId is Invalid:%s", toolTaskId.c_str());
        }
    } else if (accessCtrl->header.messageType == "TaskStatusManagement") {
    }
}

void ToolTask::RegisterToolTaskCallback(std::shared_ptr<ReactLLMTask> task,
                                        shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, const std::string& teId)
{
    task->RegisterCallback(
            LLMEventType::OnLLMFinalAnswer,
            [toolSelf = shared_from_this(), accessCtrl, teId](const LLMEventData& data) {
                auto answer = std::get<OnLLMFinalAnswerEvent>(data);
                LOG_INFO_EX("ToolTask llm response is:%s", answer.answer.c_str());
                auto cleanResponse = getCleanResponse(answer.answer);
                auto clarifyResult = ParseClarifyResult(cleanResponse);
                if (clarifyResult.clarifyStatus == CLARIFY_STATUS_FINISHED) {
                    toolSelf->GenerateContext(accessCtrl, "assistant", "任务执行成功");
                    SharedMemory::AddSharedMemory(*(toolSelf->memories), accessCtrl->header.taskID);
                    // 填充模型提取的参数
                    toolSelf->CompleteModelParams(accessCtrl, clarifyResult);
                    if (toolSelf->toolMessage.type == LOCAL) {
                        toolSelf->toolMessage.msgCallBack = AddParamsToMsgCallBack(toolSelf->toolMessage.msgCallBack, clarifyResult.paramList);
                        LOG_INFO_EX("toolMessage.msgCallBack: %s", toolSelf->toolMessage.msgCallBack.c_str());
                        toolSelf->ExecLocalTool(accessCtrl);
                        return;
                    } else if (toolSelf->toolMessage.type == MCP) {
                        toolSelf->ExecMcpTool(accessCtrl);
                        return;
                    }
                } else if (clarifyResult.clarifyStatus.empty()) {
                    TEHelper::sendUserOutputMsg(accessCtrl, "系统繁忙，请稍后进行安装重试", UserOutput_Completed, 0);
                    TEHelper::sendUserTaskCompleted(accessCtrl);
                } else {
                    // 进行追问
                    TEHelper::sendUserOutputMsg(accessCtrl, clarifyResult.question, UserOutput_Completed, 0);
                }
            });

    task->RegisterCallback(
            LLMEventType::OnLLMError,
            [accessCtrl](const LLMEventData& data) {
                TEHelper::sendUserOutputMsg(accessCtrl, "系统繁忙，请稍后进行安装重试", UserOutput_Completed, 0);
                TEHelper::sendUserTaskCompleted(accessCtrl);
            });

}

void ToolTask::GenerateContext(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, std::string role, std::string content)
{
    ShortMemory memory{
            .userName = accessCtrl->header.userName,
            .workFlowId = workFlowID,
            .taskId = taskID,
            .role = role,
            .createTime = std::to_string(CMFrm::Utils::GetCurrentTime()),
            .source = "ToolAgent",
            .content = content
    };
    CHECK_NULL_LOG_ERROR_RETURN_VOID(memories, "memories is null");
    memories->push_back(memory);
}

void ToolTask::CreateToolTask(const std::string& userInput, std::shared_ptr<TeamEnvironment>& te,
                              const mmsdk::MMPromptTemplate& prompt, const std::string& teId,
                              shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl)
{
    auto task = std::make_shared<ReactLLMTask>();
    uint32_t ignore_llm_bitmp = 0x7;
    task->SetIgnoreLlmBitmap(ignore_llm_bitmp);
    RegisterToolTaskCallback(task, accessCtrl, teId);
    if (te) {
        te->setToolTask(task);
    }
    std::string model = "Qwen3-32B-Intent";
    mmsdk::MMReasonParam param = {
            .modelServiceName = "llmlargeservice",
            .modelName = model,
            .maxToken = 1024
    };
    ShortMemory shortMemory{.userName = accessCtrl->header.userName};
    GenerateContext(accessCtrl, "user", userInput);
    reactLLMomponent->CreateTask(
            param, task,
            [=](uint32_t errorCode, std::string& taskId) -> uint32_t {
                LOG_INFO_EX("ToolTask created");
                std::vector<std::map<std::string, std::string>> ctx;
                SharedMemory::GetContext(ctx, shortMemory);
                CustomedLLMComponent->UpdateUserContext(
                        taskId, ctx, [userInput, te, taskId, prompt](uint32_t errorCode) -> uint32_t {
                            LOG_INFO_EX("LModelSerialNode update context errorCode is:%d", errorCode);
                            auto intention = mmsdk::MMUserIntention{
                                    .query = userInput + "/nothink", .modaltype = mmsdk::MMReasonModalType::MMLLM_MODAL_TYPE_TEXT};
                            auto req = mmsdk::MMLLMRequest{.intentions = {intention}, .prompt = {prompt}};
                            reactLLMomponent->RequestAsync(taskId, req, {});
                            te->setToolTaskId(taskId);
                            return 0;
                        });
                return 0;
            });
}

void ToolTask::FindTool(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    std::string teamEnvId = TEManager::getInstance().extractTeamEnvId(accessCtrl);
    auto te = TEManager::getInstance().getTEById(teamEnvId);
    std::promise<void> promise;
    std::future<void> future = promise.get_future();
    const auto& userInput = accessCtrl->eventType == SYSAGENT::EventType::TASKASSIGNMENT ?
                            accessCtrl->taskAssignment.data.userInputText : accessCtrl->userInput.data.userInputText;
    auto intention = MMUserIntention{.query = "##用户输入:" + userInput + "/nothink",
            .modaltype = MMReasonModalType::MMLLM_MODAL_TYPE_TEXT};
    LOG_INFO_EX("userInput is:%s", userInput.c_str());
    mmsdk::MMReasonParam param = {
            .modelServiceName = "llmlargeservice",
            .modelName = "Qwen3-32B-Intent",
            .maxToken = 1024
    };
    auto tmpPrompt = PromptManager::GetInstance().GetPormptTmpl("tool_selection");
    // 工具是两部分 MCP从缓存里面取构造 tool_message 只使用 名称和功能描述
    auto mcpToolList = ToolManager::getInstance().getToolList();
    std::string tool_message = "";
    for (const MCPTool& mcpTool : mcpToolList) {
        tool_message += "\n- " + mcpTool.name + " : " + mcpTool.description;
    }
    // LOCAL是从config里面拿
    auto localToolMap = ConfigManager::getInstance().GetApiMap();
    for (auto & it : localToolMap) {
        tool_message += "\n- " + it.first + " : " +  it.second.description;
    }
    LOG_INFO_EX("tool_message : %s", tool_message.c_str());
    tmpPrompt.RenderPrompt({{"tool_message", tool_message}});
    LOG_INFO_EX("tool select prompt : %s", tmpPrompt.GetContent().c_str());
    std::cout << tmpPrompt.GetContent() << std::endl;
    auto req = mmsdk::MMLLMRequest{
            .intentions = {intention},
            .prompt = {tmpPrompt}};
    std::string llmResponse = "";
    QueueManager::getInstance().EventMark("info", "ToolTask.FindTool.invokeModelBegin","success",
                                          "tool task invoke llm large model to find tool", accessCtrl);
    LLMomponent->RequestAsync(param, req,
                              [&promise, &llmResponse](uint32_t errorCode, const std::string& response) -> uint32_t {
                                  LOG_INFO_EX("errorCode is:%d, response is:%s", errorCode, response.c_str());
                                  llmResponse = response;
                                  promise.set_value();
                                  return 0;
                              });
    future.wait();
    QueueManager::getInstance().EventMark("info", "ToolTask.FindTool.invokeModelEnd","success",
                                          "tool task invoke llm large model to find tool serial RequestAsync end", accessCtrl);
    toolMessage = ParseToolInterface(llmResponse); // 根据工具选择的结果，解析接口
}

ClarifyResult ParseClarifyResult(std::string input)
{
    LOG_INFO_EX("clarify input is:%s", input.c_str());
    Json::Value root;
    JsonParser::Parse(input, root);
    ClarifyResult result;
    AisfPlat::JsonParser::GetString(root, CLARIFY_STATUS, result.clarifyStatus);
    AisfPlat::JsonParser::GetString(root, CLARIFY_QUESTION, result.question);
    if (root.isMember(CLARIFY_PARAMS) && root[CLARIFY_PARAMS].isObject()) {
        for (const auto& key : root[CLARIFY_PARAMS].getMemberNames()) {
            std::string value = "";
            AisfPlat::JsonParser::GetString(root[CLARIFY_PARAMS], key, value);
            result.paramList[key] = value;
            LOG_INFO_EX("params key is:%s, value is:%s", key.c_str(), value.c_str());
        }
    }
    return result;
}

// 发送TaskStatusReport消息
bool ToolTask::SendTaskStatusReport(int statusCode, std::string statusReport, std::string teId)
{
    LOG_INFO_EX("send TaskStatusReport to APP, statusCode: %d, statusReport: %s", statusCode, statusReport.c_str());
    std::cout << "send TaskStatusReport to APP, statusCode: " << statusCode << ", statusReport: " << statusReport
              << std::endl;

    auto te = TEManager::getInstance().getTEById(teId);
    if (!te) {
        LOG_ERR_EX("Failed to get te");
        return false;
    }

    auto wf = te->getWorkFlow(taskID);
    if (!wf) {
        LOG_ERR_EX("Failed to get workflow");
        return false;
    }

    auto msg = TEHelper::newTaskStatusReport(teId, statusCode, statusReport);
    if (!msg) {
        LOG_ERR_EX("Failed to get te");
        return false;
    }
    msg->setSender("SystemAgent");
    msg->setReceiver(wf->GetTopSender());
    msg->setTaskID(taskID);
    msg->setClientExField(wf->GetClientExField());
    wf->ChangeStatus(taskID, msg);
    LOG_INFO_EX("send TaskStatusReport to workflow");
    return true;
}

ToolMessage ToolTask::ParseToolInterface(const std::string& query)
{
    std::string result = query;
    if (result.size() >= NUMBER_TWO && result.front() == '[' && result.back() == ']') {
        result = result.substr(NUMBER_ONE, result.size() - NUMBER_TWO);
    }
    if (result.size() >= NUMBER_TWO && result.front() == '"' && result.back() == '"') {
        result = result.substr(NUMBER_ONE, result.size() - NUMBER_TWO);
    }
    auto apiMap = ConfigManager::getInstance().GetApiMap();
    if (apiMap.find(result) != apiMap.end()) {
        toolType = "LOCAL"; // 本地工具
        toolName = result;
        return apiMap[result];
    }
    // 可能是MCP工具
    const auto toolList = ToolManager::getInstance().getToolList();
    auto toolMsgMap = ToolManager::getInstance().ConvertMCPToolsToToolMessages(toolList);
    if (toolMsgMap.find(result) != toolMsgMap.end()) {
        toolType = "MCP";
        toolName = result;
        return toolMsgMap[result];
    }
    return ToolMessage{};
}

void ToolTask::CompleteCommonParams(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    Json::Value root;
    JsonParser::Parse(accessCtrl->toString(), root);
    std::map<std::string, std::string> params{};
    JsonParser::ParseJsonToMap(root,  "", params);

    Json::Value payLoad;
    accessCtrl->taskAssignment.payload.ToJson(payLoad);
    JsonParser::ParseJsonToMap(payLoad,  "", params);

    LOG_INFO_EX("start output params");
    for (auto &param : params) {
        LOG_INFO_EX("key:%s, value:%s", param.first.c_str(), param.second.c_str());
    }

    // 遍历toolInterface
    int index = 1;
    std::string templateStr1 = R"({{api_index}}、 {{api_name}}参数：必填字段，{{api_description}}；)";
    for (auto& item : toolMessage.toolInterfaces) {
        for (auto& param : item.apiParams) {
            if (param.second.source.empty()) {
                param.second.needModelExtract = true;
                needExtractParams = true;
                // 封装APIDescriptions
                auto res1 = ReplacePlaceholders(templateStr1, "{{api_index}}", std::to_string(index++));
                auto res2 = ReplacePlaceholders(res1, "{{api_name}}", param.first);
                auto result = ReplacePlaceholders(res2, "{{api_description}}", param.second.description);
                apiDescriptions += result;
            } else {
                LOG_INFO_EX("source is:%s, params is:%s", item.apiParams[param.first].source.c_str(),
                            params[item.apiParams[param.first].source].c_str());
                item.apiParams[param.first].source = params[item.apiParams[param.first].source];
            }
        }
    }
}

void ToolTask::CompleteModelParams(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, ClarifyResult& clarifyResult)
{
    // 只允许填充需要填入的参数
    LOG_INFO_EX("start complete model params");
    auto paramList = clarifyResult.paramList;
    for (auto& pair : paramList) {
        LOG_INFO_EX("pair first : %s, second : %s", pair.first.c_str(), pair.second.c_str());
    }
    for (auto& item : toolMessage.toolInterfaces) {
        for (auto& param : item.apiParams) {
            if (paramList.find(param.first) != paramList.end() && param.second.needModelExtract) {
                LOG_INFO_EX("param.first : %s", param.first.c_str());
                LOG_INFO_EX("paramList[param.first] : %s", paramList[param.first].c_str());
                item.apiParams[param.first].source = paramList[param.first];
            }
        }
    }
}

std::string ReplacePlaceholders(const std::string& templateStr,
                                const std::string& placeholder, const std::string& value)
{
    std::string result = templateStr;
    size_t pos = 0;
    while ((pos = result.find(placeholder, pos)) != std::string::npos) {
        result.replace(pos, placeholder.length(), value);
        pos += value.length();
    }
    return result;
}

void ToolTask::ExecMcpTool(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    // todo，调用mcp工具
    LOG_INFO_EX("start exec MCP tool");
    /**
     * 工具名称: toolMessage.id
     * 参数列表：toolMessage.toolInterfaces[0].apiParams
     */
    Json::Value params;
    for (const auto& interface : toolMessage.toolInterfaces) {
        for (const auto& param : interface.apiParams) {
            params[param.first] = param.second.source;
        }
    }
    LOG_INFO_EX("params is :%s", JsonParser::ToRawString(params).c_str());
    Json::Value result;
    if (!SYSAGENT::Tool::CallTool(toolMessage.id, params, result)) {
        LOG_ERR_EX("ToolTask invoke tool failed:%s", toolMessage.id.c_str());
        TEHelper::sendUserOutputMsg(accessCtrl, "工具调用失败", "", UserOutput_Completed, 0);
        TEHelper::sendNewTaskStatusReport(accessCtrl);
        return;
    }
    std::string userOutputObject;
    if ((result.isMember("data") && result["data"].isString()) ||
        !result["errCode"].isInt() || result["errCode"].asInt()) {
        userOutputObject = result["data"].asString();
    }
    ToolTask::SendToolUserOutputMsg(accessCtrl, userOutputObject);
    TEHelper::sendNewTaskStatusReport(accessCtrl);
}

void ToolTask::ExecLocalTool(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl)
{
    LOG_INFO_EX("start exec Local tool");
    SendToolUserOutputMsg(accessCtrl, toolMessage.msgCallBack);
    TEHelper::sendNewTaskStatusReport(accessCtrl);
}

void ToolTask::SendToolUserOutputMsg(shared_ptr<SYSAGENT::AccessCtrl> accessCtrl, const std::string& callback)
{
    Json::Value root;
    JsonParser::Parse(callback, root);
    ToolTaskCallback toolTaskCallback{};
    AisfPlat::JsonParser::GetUInt(root, "code", toolTaskCallback.code);
    AisfPlat::JsonParser::GetUInt(root, "retCode", toolTaskCallback.retCode);
    AisfPlat::JsonParser::GetString(root, "msg", toolTaskCallback.msg);
    if (root.isMember("data")) {
        toolTaskCallback.data = root["data"];
    }

    string userMsg = toolTaskCallback.msg;
    if (toolTaskCallback.retCode != 200) {
        LOG_ERR_EX("Tool call failed, code: %d, retCode: %d, error msg: %s",
                   toolTaskCallback.code, toolTaskCallback.retCode, toolTaskCallback.msg.c_str());
        userMsg = "系统繁忙，请稍后进行安装重试";
        std::string msg = toolTaskCallback.msg;
        auto retCode = toolTaskCallback.retCode;
        if (retCode == 501) {
            userMsg = "该应用不支持秒装";
        } else if (retCode == 401) {
            userMsg = msg;
        }
    }

    switch (toolTaskCallback.code) {
        case ToolRetCode::FIVE:
            TEHelper::sendUserOutputMsg(accessCtrl, userMsg, "", UserOutput_Completed, 0);
            break;
        case ToolRetCode::FOUR:
            SendSpecifiedUserOutputMsg(accessCtrl, "", callback, UserOutput_Completed, 0, "APP");
            break;
        default:
            TEHelper::sendUserOutputMsg(accessCtrl, "", callback, UserOutput_Completed, 0);
            break;
    }
}

void ToolTask::SendSpecifiedUserOutputMsg(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl,
                                          const std::string& content, const std::string& outputObject,
                                          const std::string& status, int flag, const std::string& receiver)
{
    std::shared_ptr<TeamEnvironment> te = TEManager::getInstance().getTEById(accessCtrl->header.teamEnvironmentID);
    auto outputMsg = TEHelper::newUserOutput(te, content, outputObject, status, flag);
    if (!outputMsg) {
        LOG_ERR_EX("outputMsg pointer is null");
        return;
    }
    outputMsg->setSender("SystemAgent");
    outputMsg->setReceiver(receiver);
    string taskID = accessCtrl->header.taskID;
    outputMsg->setTaskID(taskID);
    outputMsg->setClientExField(accessCtrl->header.clientExField);
    QueueManager::getInstance().postWriteQueue(outputMsg, te->session);
}