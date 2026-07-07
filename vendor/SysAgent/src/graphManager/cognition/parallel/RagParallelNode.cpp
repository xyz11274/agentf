// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
#include "graphManager/cognition/parallel/RagParallelNode.h"
#include "aisf_platform/common_define.h"
#include "graphManager/GraphData.h"
#include "graphManager/GraphManager.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/base/BaseNode.h"
#include "memory/GaussClient.h"
#include "memory/SafetyFence.h"
#include <memory>
#include <string>

using namespace DM::RAG;

float INTENT_THRESHOLD = 0.3;
mmagentflow::MMNodeFunctionStatus RagParallelNode::Run(std::shared_ptr<mmagentflow::MMMessage> msg)
{
    LOG_INFO_EX("RequestId: %d, Run: %s", msg->GetRequestId(), name_.c_str());
    GraphUtils::AddDebugInfo(msg, name_);
    auto access = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
    std::string userInput = access->taskAssignment.data.userInputText;
    auto queryOption = QueryOption::Builder(TableQueryType::VECTOR)
                           .WithBaseSchemaName(SCEMA_NAME)
                           .WithVectorDistanceAlgorithm(VectorDistanceAlgorithm::L2)
                           .WithVectorField("query_vector")
                           .WithVectorTopK(1)
                           .WithBaseOrder(QueryOption::QueryOrder::ASC)
                           .WithBaseFilters(FilterOption::Builder()
                                                .WithFieldName("scene_type")
                                                .WithEQ()
                                                .WithValue(make_shared<VarCharValue>("2"))
                                                .Build())
                           .WithVectorQuery(userInput)
                           .WithBaseSelectFields({"scene_type", "query", "content"})
                           .WithVectorReturnDistance()
                           .Build();
    GaussClient::Instance()->SearchAsync(
        "safetyfence", queryOption, [msg](DBOperateCode code, const std::vector<std::vector<std::string>>& result) {
            if (!result.empty() && stof(result[0][0]) < INTENT_THRESHOLD) {
                // 解析params
                Json::Value root;
                std::map<std::string, std::string> params;
                JsonParser::Parse(result[0][NUMBER_THREE].c_str(), root);
                if (root.isMember("params")) {
                    Json::Value::Members members = root["params"].getMemberNames();
                    for (const auto& member : members) {
                        params.insert({member.c_str(), root["params"][member].asString()});
                    }
                }
                GraphUtils::AddMessageData(
                    msg, "ParallelNodeResult",
                    ParallelReault(RAGResult{true, "",
                                             SYSAGENT::SafetyFence::parseIntentName(result[0][3]), "", params}));
            } else {
                auto parallelResult = ParallelReault(RAGResult{false, "", "", ""});
                GraphUtils::AddMessageData(msg, "ParallelNodeResult", parallelResult);
            }
            GraphUtils::NotifyAsyncEvent(msg, result);
        });
    GraphUtils::PrintDebugInfo(msg);
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_WAIT_ASYNC;
}

mmagentflow::MMNodeFunctionStatus RagParallelNode::AsyncRun(std::shared_ptr<mmagentflow::MMEvent> event)
{
    return mmagentflow::MMNodeFunctionStatus::FUNCTION_COMPLETE;
}