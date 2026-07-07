// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_GRAPHDATA_H
#define SYSAGENT_GRAPHDATA_H

#include "WorkFlow.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "aisf_platform/parser/json_parser.h"
#include "json/value.h"
#include "mm_message.h"
#include <memory>
#include <mutex>
#include <utility>
#include <variant>
#include <vector>
#include <taskManage/Task.h>

// message data
#define GRAPH_DATA_ACCESSCTRL "ACCESSCTRL"
#define GRAPH_DATA_ISDECISION "ISDECISION"
#define GRAPH_DATA_PLANEXAMPLE "PLANEXAMPLE"
#define GRAPH_DATA_PARENTGRAPHMSG "PARENTGRAPHMSG"
#define GRAPH_DATA_ParallelNodeResult "ParallelNodeResult"
#define GRAPH_DATA_UnifyIntentNodeResult "UnifyIntentNodeResult"
#define GRAPH_DATA_UnifySlowIntentRecResult "UnifySlowIntentRecResult"
#define GRAPH_DATA_MEMORY "GRAPH_DATA_MEMORY"
#define GRAPH_DATA_RecallMemory "GRAPH_DATA_RecallMemory"
#define GRAPH_DATA_RewriteQ "GRAPH_DATA_RewriteQ"
#define GRAPH_DATA_ExtractParam "ExtractParam"
#define GRAPH_DATA_Classifying "Classifying"
#define GRAPH_DATA_Adjudication "Adjudication"
#define GRAPH_WORKFLOW "GRAPH_WORKFLOW"

#define GRAPH_DATA_NEXTNODE "NEXTNODE"

// node name
#define GRAPH_NODE_STARTNNODE "StartNode"
#define GRAPH_NODE_PerceptionNode "PerceptionNode"
#define GRAPH_NODE_RuleNode "RuleNode"
#define GRAPH_NODE_ParallelStartNode "ParallelStartNode"
#define GRAPH_NODE_RagParallelNode "RagParallelNode"
#define GRAPH_NODE_SModelParallelNode "SModelParallelNode"
#define GRAPH_NODE_LModelParallelNode "LModelParallelNode"
#define GRAPH_NODE_DecisionNode "DecisionNode"
#define GRAPH_NODE_PlanNode "PlanNode"
#define GRAPH_NODE_ExecuteNode "ExecuteNode"
#define GRAPH_NODE_EndNode "EndNode"
#define GRAPH_NODE_SubStartNode "SubStartNode"
#define GRAPH_NODE_ClassifyingNode "ClassifyingNode"
#define GRAPH_NODE_ExtractParamNode "ExtractParamNode"
#define GRAPH_NODE_AdjudicationNode "AdjudicationNode"
#define GRAPH_NODE_SubEndNode "SubEndNode"

using AccessCtrlPtr = std::shared_ptr<SYSAGENT::AccessCtrl>;
using MMMessagePtr = std::shared_ptr<mmagentflow::MMMessage>;

struct NextNode {
    std::vector<std::string> next;
};

struct PlanExample {
    std::string from;
    std::vector<IntentTask> tasks;

    PlanExample() {}
    std::vector<AgentData> agentDatas; // 新增agent信息列表
    PlanExample(std::string from, std::vector<IntentTask> tasks) : from(std::move(from)), tasks(std::move(tasks)) {}
    PlanExample(std::string from, std::vector<IntentTask> tasks, std::vector<AgentData> agentDatas) :
            from(std::move(from)), tasks(std::move(tasks)), agentDatas(std::move(agentDatas)) {}
    std::string ToString() {
        Json::Value root;
        root["from"] = from;
        Json::Value tasksJson(Json::arrayValue);
        for (const auto& task : tasks) {
            // task tostring
            Json::Value taskJson;
            taskJson["agentName"] = task.agentName;
            taskJson.append(taskJson);
        }
        root["tasks"] = tasksJson;
        return JsonParser::ToRawString(root);
    }
};

struct DecisionStat {
    bool isDecision;
    bool containQA{false};
    std::mutex mut;
    std::pair<std::string, bool> reWriteQ{"", false};
    std::pair<std::string, bool> recallMemory{"", false};
    std::pair<std::string, bool> extractTool{"", false};
    PlanExample planExample{};
    std::vector<shared_ptr<AgentTask>> agentTasks{};
    std::vector<IntentTask> additionalIntentTasks{};
    std::string workflowID;
    std::string reWriteQTaskID;
    std::string extractToolTaskID;
    DecisionStat() = default;
    DecisionStat(bool decision) : isDecision(std::move(decision)) {}
    DecisionStat(bool decision, std::string workflowID) : isDecision(std::move(decision)), workflowID(std::move(workflowID)) {}
};

struct ExternalJudgmentResult {
    std::string result;
    std::string reason;

    bool JsonLoad(const std::string& jsonStr)
    {
        Json::Value jsonReq;
        if (!JsonParser::Parse(jsonStr, jsonReq)) {
            LOG_ERR_EX("invalid json str: %s", jsonStr.c_str());
            return false;
        }
        if (JsonParser::GetString(jsonReq, "result", this->result) &&
            JsonParser::GetString(jsonReq, "reason", this->reason)) {
            return true;
        }
        LOG_ERR_EX("get result or reason failed, jsonStr: %s", jsonStr.c_str());
        return false;
    }
};

struct Adjudication {
    bool match;
    bool paramsMatch;
};

struct RAGResult {
    bool similar;
    std::string category;
    std::string intent;
    std::string data;
    std::map<std::string, std::string> params;
};

struct SModelResult {
    bool match;
    bool paramsMatch;
    std::string categoryId;
    std::string intent;
    std::map<std::string, std::string> params;
};

struct LModelResult {
    std::vector<IntentTask> tasks;
};

struct QAToolExtractResult {
    bool success;
};

enum class ParallelReaultType { RAG, SModel, LModel, QAToolExtract, INVALID};

struct ParallelReault {
    ParallelReaultType type;
    std::variant<RAGResult, SModelResult, LModelResult, QAToolExtractResult> result;
    ParallelReault() : type(ParallelReaultType::INVALID) {}
    ParallelReault(QAToolExtractResult result) : type(ParallelReaultType::QAToolExtract), result(result) {}
    ParallelReault(RAGResult result) : type(ParallelReaultType::RAG), result(result) {}
    ParallelReault(SModelResult result) : type(ParallelReaultType::SModel), result(result) {}
    ParallelReault(LModelResult result) : type(ParallelReaultType::LModel), result(result) {}
};

enum class UnifyIntentResultType { FastIntent, SlowIntent, DEFAULT};

struct UnifyIntentModelResult {
    std::vector<IntentTask> tasks;
};

struct UnifyIntentResult {
    UnifyIntentResultType type;
    UnifyIntentModelResult result;
    UnifyIntentResult() : type(UnifyIntentResultType::DEFAULT), result{} {}
    UnifyIntentResult(UnifyIntentModelResult result,
                      UnifyIntentResultType type) : type(type), result(std::move(result)) {}
};


struct EventMarkMsg {
    std::string timeStamp;
    std::string logLevel;
    std::string details;
    std::string eventType;
    std::string eventName;
    std::string status;
    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl;
    // 有参构造函数，eventType 有默认值 "EM"
    EventMarkMsg(
            const std::string& _timeStamp,
            const std::string& _logLevel,
            const std::string& _details,
            const std::string& _eventName,
            const std::string& _status,
            const std::shared_ptr<SYSAGENT::AccessCtrl>& _accessCtrl,
            const std::string& _eventType = "EM")
            : timeStamp(_timeStamp)
            , logLevel(_logLevel)
            , details(_details)
            , eventType(_eventType)
            , eventName(_eventName)
            , status(_status)
            , accessCtrl(_accessCtrl)
    {
        // 可以在这里添加初始化逻辑，比如验证参数是否为空等
    }
};

#endif  // SYSAGENT_GRAPHDATA_H