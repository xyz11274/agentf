// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_GRAPHUTILS_H
#define SYSAGENT_GRAPHUTILS_H

#include "graphManager/base/BaseNode.h"
#include "mm_agent_graph.h"
#include "mm_message.h"
#include "mm_message_factory.h"
#include <chrono>
#include <memory>
#include "aisf_platform/common_define.h"
#include "mm_node.h"
#include "graphManager/GraphData.h"
#define GRAPH_DATA_DEBUGINFO "DebugInfo"

#define STATIC_GRAPH_INTENT "static_graph_instent"
#define STATIC_GRAPH_SUB_INTENT "static_graph_sub_instent"

#define AGENT_GRAPH_INTENT "agent_graph_instent"
#define AGENT_GRAPH_SUB_INTENT "agent_graph_sub_instent"

struct DebugInfo {
    std::string nodename;

    // 节点进入时间
    std::chrono::time_point<std::chrono::steady_clock> start;
};

class GraphUtils {
public:
    // 删除构造函数，防止实例化
    GraphUtils() = delete;
    GraphUtils(const GraphUtils&) = delete;
    GraphUtils(GraphUtils&&) = delete;
    ~GraphUtils() = delete;

public:
    static std::shared_ptr<mmagentflow::MMMessage> NewMessage(std::shared_ptr<mmagentflow::MMAgentGraph> graph)
    {
        return mmagentflow::MMMessageFactory::GetInstance().NewMessage(graph, requestId++);
    }

    template <typename T>
    static void AddMessageData(std::shared_ptr<mmagentflow::MMMessage> message, std::string key, T data)
    {
        message->GetData()[key] = std::make_shared<BaseData<T>>(data);
    }

    template <typename T>
    static T GetMessageData(std::shared_ptr<mmagentflow::MMMessage> message, std::string key)
    {
        // 检查key是否存在
        auto p = std::dynamic_pointer_cast<BaseData<T>>(message->GetData()[key]);
        if (p == nullptr) {
            LOG_ERR_EX("GraphUtils::GetMessageData, key : [%s] is nullptr", key.c_str());
            return T();
        }
        return std::dynamic_pointer_cast<BaseData<T>>(message->GetData()[key])->getValue();
    }

    template <typename T>
    static void NotifyAsyncEvent(std::shared_ptr<mmagentflow::MMMessage> msg, T data)
    {
        auto context = msg->GetContext();
        auto graph = std::any_cast<std::shared_ptr<mmagentflow::MMAgentGraph>>(context.at(MM_KEY_CURRENT_AGENT_GRAPH));
        auto eventMsg = mmagentflow::MMMessageFactory::GetInstance().NewEvent(
                msg, std::any_cast<uint64_t>(context.at(MM_KEY_CURRENT_NODE_ID)));
        eventMsg->SetData(std::make_shared<BaseData<T>>(data));
        graph->NotifyAsyncEvent(eventMsg);
    }

    static void AddDebugInfo(const std::shared_ptr<mmagentflow::MMMessage>& message, const std::string& nodename)
    {
        auto debugInfo = DebugInfo{.nodename = nodename, .start = std::chrono::steady_clock::now()};
        AddMessageData(message, GRAPH_DATA_DEBUGINFO, debugInfo);
    }

    static void PrintDebugInfo(std::shared_ptr<mmagentflow::MMMessage> message, std::string description = "")
    {
        auto debugInfo = GetMessageData<DebugInfo>(message, GRAPH_DATA_DEBUGINFO);
        auto end = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - debugInfo.start).count();
        auto timeNow = std::chrono::time_point_cast<std::chrono::milliseconds>(end).time_since_epoch().count();
        LOG_INFO_EX("%s [DEBUG] node: %s, time now: %ld (ms), use count: %ld", description.c_str(),
                    debugInfo.nodename.c_str(), timeNow, ms);
    }
    static std::vector<std::string> getIndexArray(const std::string& taskID)
    {
        std::vector<std::string> result;
        size_t start = 0, end = 0;

        while ((end = taskID.find('_', start)) != std::string::npos) {
            // 将子字符串转换为整数并添加到结果数组中
            result.push_back(taskID.substr(start, end - start));
            start = end + 1;
        }
        // 处理最后一个子字符串
        result.push_back(taskID.substr(start));
        return result;
    }

private:
    inline static uint64_t requestId = 1;
};

#endif  // SYSAGENT_GRAPHUTILS_H