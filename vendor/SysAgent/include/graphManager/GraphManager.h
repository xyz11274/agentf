// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_GRAPHMANAGER_H
#define SYSAGENT_GRAPHMANAGER_H

#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "mm_agent_graph.h"
#include "mm_edge_factory.h"
#include "mm_graph.h"
#include "mm_node.h"
#include <map>
#include <memory>
#include <shared_mutex>

#define NewMMEdge mmagentflow::MMEdgeFactory::GetInstance().NewMMEdge

class GraphManager {
public:
    using MMAgentGraphPtr = std::shared_ptr<mmagentflow::MMAgentGraph>;

    static GraphManager& GetInstance()
    {
        static GraphManager instance;
        return instance;
    }

    void Init();

    template <typename T>
    std::shared_ptr<mmagentflow::MMNode> NewNode(std::string name, mmagentflow::MMNodeConfig config);

    void RegisterNode(std::string name, std::shared_ptr<mmagentflow::MMNode>);

    MMAgentGraphPtr GetAgentGraph(const std::string& name);

    std::vector<std::shared_ptr<mmagentflow::MMNode>> ConditionFunc(std::shared_ptr<mmagentflow::MMMessage> msg) const;

private:
    std::map<std::string, std::shared_ptr<mmagentflow::MMGraph>> static_graphs_;
    std::map<std::string, MMAgentGraphPtr> agent_graphs_;
    std::map<std::string, std::shared_ptr<mmagentflow::MMNode>> nodes_;
};

#endif  // SYSAGENT_GRAPHMANAGER_H