//
// Created by c00888021 on 2026/3/15.
//

#include "graphManager/cognition/parallel/ExtractToolNode.h"
#include "memory/ContextManager.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/GraphData.h"
#include "model/PromptManager.h"
#include "model/ReactLLMTask.h"
#include "config/Config.h"
#include "taskManage/TeamEnvironment.h"
#include "taskManage/TeHelper.h"
#include "memory/Location.h"
#include <algorithm>
#include "framework_common/utils/file_util.h"

#define EXTRACT_TOOL_NODE_TIMER "EXTRACT_TOOL_NODE_TIMER"

using namespace ContextManager;

ExtractToolNode::ExtractToolNode(const std::string& nodeName,
                                 const mmagentflow::MMNodeConfig& nodeConfig) : BaseNode(nodeName, nodeConfig)
{
}

std::shared_ptr<mmagentflow::MMNode> ExtractToolNode::Clone()
{
    return std::make_shared<ExtractToolNode>(name_, config_);
}

mmagentflow::MMNodeFunctionStatus ExtractToolNode::CancelAsyncRun(std::shared_ptr<mmagentflow::MMMessage> msg)
{
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}

std::shared_ptr<mmagentflow::MMMessage> ExtractToolNode::DataMsgMerge(
        const std::map<std::string, std::shared_ptr<mmagentflow::MMMessage>>& inputmsgs)
{
    return inputmsgs.begin()->second;
}

void ExtractModelInfer(std::shared_ptr<ReactLLMTask> task,
                       std::shared_ptr<mmagentflow::MMMessage> msg,
                       mmsdk::MMLLMRequest req,
                       std::vector<std::map<std::string, std::string>> mixedContext)
{
    std::string model = ConfigManager::getInstance().getEnvInfo("REWRITE_QUERY_MODEL", "");
    std::string modelServiceName = ConfigManager::getInstance().getEnvInfo("REWRITE_QUERY_MODEL_TYPE", "llmlargeservice");
    mmsdk::MMReasonParam param = {
        .modelServiceName = modelServiceName, .modelName = model, .temperature = 0.001, .maxToken = 1024
    };
    reactLLMomponent->CreateTask(param, task, [msg, req, mixedContext](uint32_t errorCode, const std::string& taskId) -> uint32_t {
        if (errorCode != mmsdk::MMErrorCode::SUCCESS) {
            LOG_ERR_EX("create ExtractTool Task failed");
            GraphUtils::NotifyAsyncEvent(msg, std::pair<std::string, bool>("", true));
            return errorCode;
        } else {
            auto decisionStat = GraphUtils::GetMessageData<std::shared_ptr<DecisionStat>>(msg, GRAPH_DATA_ISDECISION);
            decisionStat->extractToolTaskID = taskId;
            CustomedLLMComponent->UpdateUserContext(taskId, mixedContext, [=](uint32_t errorCode) -> uint32_t {
                reactLLMomponent->RequestAsync(taskId, req, {});
                return 0;
            });
        }
        return 0;
    });
}

void RegisterExtractToolModelInfer(std::shared_ptr<mmagentflow::MMMessage> msg, ContextRequest request,
                                   const std::string& workflowID, UserInfo userInfo, const std::string& userInput)
{
    ContextManager::GetContext(request, [=](bool isSuccess, std::shared_ptr<ContextResult> result) {
        GraphUtils::PrintDebugInfo(msg, "Recall Short Memory");
        CHECK_TRUE_LOG_ERROR(!result, "Recall short memory is empty!");
        std::string sysPromptName = "extract_tool_name_system";
        std::string userPromptName = "extract_tool_name_user";
        if (CMFrm::Utils::CheckFileExist("/opt/coremind/conf/use_huoshan.json")) {
            sysPromptName = "huoshan_extract_tool_name_system";
            userPromptName = "huoshan_extract_tool_name_user";
        }
        auto sysPrompt = PromptManager::GetInstance().GetPormptTmpl(sysPromptName);
        auto userPrompt = PromptManager::GetInstance().GetPormptTmpl(userPromptName);
        userPrompt.RenderPrompt({{"USER_QUESTION", userInput}});
        auto intention = mmsdk::MMUserIntention{.query = userPrompt.GetContent(),
                                                .modaltype = mmsdk::MMReasonModalType::MMLLM_MODAL_TYPE_TEXT};
        auto req = mmsdk::MMLLMRequest{.intentions = {intention}, .prompt = {sysPrompt}};
        std::string contextLog = "[" + AisfPlat::Utils::GetCurrentTimestampInCustomFormat() + "]Question:" + userInput;
        contextLog += "\n======= ReWriteQuery context =======\n";
        auto mixedContext = GetMixedContext(result);
        for (auto &memory : mixedContext) {
            contextLog += memory["role"] + ": " + memory["content"] + "\n";
        }
        contextLog += "======= ExtractTool SysPrompt =======\n" + sysPrompt.GetContent();
        contextLog += "======= ExtractTool Userprompt =======\n" + intention.query;
        std::string timeStr = workflowID;
        CommonFunc::AppendDebugInfoToFile(userInfo.userName, timeStr, contextLog);
        auto task = std::make_shared<ReactLLMTask>();
        task->RegisterCallback(LLMEventType::OnLLMFinalAnswer, [=](const LLMEventData& data) {
            auto answer = std::get<OnLLMFinalAnswerEvent>(data);
            std::string finalAnswer = trimThinkFlag(answer.answer);
            finalAnswer = CommonFunc::ExtractJsonString(finalAnswer);
            LOG_INFO_EX("Trim ExtractTool answer is:%s", finalAnswer.c_str());
            GraphUtils::NotifyAsyncEvent(msg, std::pair<std::string, bool>(finalAnswer, true));
            std::string prefix = "======= ExtractTool result =======\n";
            CommonFunc::AppendDebugInfoToFile(userInfo.userName, timeStr, prefix + finalAnswer);
        });
        task->RegisterCallback(LLMEventType::OnLLMError, [=](const LLMEventData& data) {
            LOG_INFO_EX("ExtractTool model error");
            GraphUtils::NotifyAsyncEvent(msg, std::pair<std::string, bool>("", true));
        });
        ExtractModelInfer(task, msg, req, mixedContext);
    });
}

mmagentflow::MMNodeFunctionStatus ExtractToolNode::Run(std::shared_ptr<mmagentflow::MMMessage> msg)
{
    SetTimeoutTask(msg, EXTRACT_TOOL_NODE_TIMER, CMFrm::Constants::Const8, [msg]() {
        LOG_ERR_EX("EXTRACT_TOOL timeout!");
        GraphUtils::NotifyAsyncEvent(msg, std::pair<std::string, bool>("", true));
    });
    GraphUtils::AddDebugInfo(msg, name_);
    auto accessCtrl = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
    auto decision = GraphUtils::GetMessageData<std::shared_ptr<DecisionStat>>(msg, GRAPH_DATA_ISDECISION);
    auto userInput = decision->reWriteQ.first.empty() ? accessCtrl->extractInput() : decision->reWriteQ.first;
    UserInfo userInfo = TEHelper::extractUserInfo(accessCtrl);
    ContextRequest request {
        .userId = userInfo.userName,
        .sessionId = "",
        .currentAgent = "sysAgent",
        .queryType = "all",
        .query = userInput,
        .rewriteQuery = userInput,
        .limitQARounds = 20,
        .maxQATokens = 2048,
    };
    auto decisionStat = GraphUtils::GetMessageData<std::shared_ptr<DecisionStat>>(msg, GRAPH_DATA_ISDECISION);
    std::string workflowID = decisionStat ? decisionStat->workflowID : "";
    RegisterExtractToolModelInfer(msg, request, workflowID, userInfo, userInput);
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_WAIT_ASYNC;
}

mmagentflow::MMNodeFunctionStatus ExtractToolNode::AsyncRun(std::shared_ptr<mmagentflow::MMEvent> event)
{
    auto data = std::dynamic_pointer_cast<BaseData<std::pair<std::string, bool>>>(event->GetData())->getValue();
    auto msg = event->GetMessage();
    LOG_INFO_EX("Extract Tool AsyncRun data is:%s, result is:%d", data.first.c_str(), data.second);
    GraphUtils::PrintDebugInfo(msg, name_);
    auto accessCtrl = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
    auto decisionStat = GraphUtils::GetMessageData<std::shared_ptr<DecisionStat>>(msg, GRAPH_DATA_ISDECISION);
    auto taskId = decisionStat->extractToolTaskID;
    if (!taskId.empty()) {
        reactLLMomponent->DestroyTask(taskId, [](uint32_t errorCode, std::string taskId) -> uint32_t {
            LOG_INFO_EX("destroy ExtractTool task: %s, %d", taskId.c_str(), errorCode);
            return 0;
        });
    }
    decisionStat->extractTool = {data.first, data.second};
    auto timeoutTask = GraphUtils::GetMessageData<std::shared_ptr<CMFrm::Timer::TimerTask>>(msg, EXTRACT_TOOL_NODE_TIMER);
    if (timeoutTask != nullptr && timeoutTask->GetTask() != nullptr) {
        GraphUtils::AddMessageData(msg, EXTRACT_TOOL_NODE_TIMER, nullptr);
        CMFrm::Timer::TimerManager::GetTimerManager()->DeleteTimerTask(timeoutTask->GetTaskId());
        LOG_INFO_EX("Delete EXTRACT_TOOL_NODE_TIMER TimeoutTask success!");
    } else {
        LOG_ERR_EX("No need to delete EXTRACT_TOOL_NODE_TIMER TimeoutTask");
    }
    GraphUtils::AddMessageData(msg, GRAPH_DATA_ParallelNodeResult, ParallelReault{QAToolExtractResult()});
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}

vector<IntentTask> ExtractToolNode::ParseToolSelection(const std::string& request)
{
    Json::Value root;
    JsonParser::Parse(request, root);
    std::vector<IntentTask> qaTasks;
    if (root.isArray()) {
        for (auto& it : root) {
            IntentTask task;
            if (JsonParser::GetString(it, "name", task.data.toolName) &&
                JsonParser::GetString(it, "query", task.requestContent)) {
                task.agentName = "QAAgent";
                qaTasks.push_back(task);
            }
        }
    }
    if (!CMFrm::Utils::CheckFileExist("/opt/coremind/conf/use_huoshan.json")) {
        return qaTasks;
    }

    // webSearch 合并
    IntentTask webSearchTask("QAAgent", "", "");
    webSearchTask.data.toolName = "web_search_query";
    std::vector<IntentTask> mergedTasks;
    std::vector<IntentTask> ltTasks;
    std::vector<IntentTask> nativeTasks;
    bool containsWebSearchTask = false;
    // 如果只有一个联通任务，还需要再创建一个webSearach任务
    for (auto& task : qaTasks) {
        if (task.data.toolName == "liantong_query") {
            webSearchTask.requestContent += task.requestContent + " ";
            ltTasks.push_back(task);
            containsWebSearchTask = true;
        } else if (task.data.toolName == "native_llm_chat") {
            nativeTasks.push_back(task);
        } else if (task.data.toolName == "web_search_query") {
            webSearchTask.requestContent += task.requestContent + " ";
            containsWebSearchTask = true;
        }
    }
    mergedTasks.insert(mergedTasks.begin(), nativeTasks.begin(), nativeTasks.end());
    if (containsWebSearchTask) {
        mergedTasks.push_back(webSearchTask);
    }
    mergedTasks.insert(mergedTasks.end(), ltTasks.begin(), ltTasks.end());
    for (auto& task : mergedTasks) {
        task.data.params["isHaveSendHuoShan"] = containsWebSearchTask ? "1" : "0";
    }
    return mergedTasks;
}