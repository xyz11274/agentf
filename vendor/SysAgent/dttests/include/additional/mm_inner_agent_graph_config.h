/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2025. All rights reserved.
 * Description:
 * Date: 2025/3/28
 */

#ifndef MMAGENTGRAPHCPP_MM_INNER_AGENT_GRAPH_CONFIG_H
#define MMAGENTGRAPHCPP_MM_INNER_AGENT_GRAPH_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

typedef uint32_t (*WriteLogFunc)(uint32_t level, const std::string &str);

namespace mmagentflow {
    struct MMCurrNode {
        std::string mmcurrNodeName;
    };
    struct MMCurrEdge {
        std::string mmCurredgeName;
    };

    struct MMCurrGraph {
        std::string mmCurrGraphName;
        std::vector<MMCurrNode> mmCurrNodes;
        std::vector<MMCurrEdge> mmCurredgeNames;
    };
    struct MMCurrGraphs {
        std::vector<MMCurrGraph> curMMGraphs;
    };

    struct MMCurrRequest {
        uint64_t requestId;
    };

    struct MMCurrAgentGraphSchedParam {
        uint32_t group;
        uint32_t priority;
    };

    struct MMCurrAgentGraph {
        MMCurrAgentGraphSchedParam mmCurrAgentGraphSchedParam;
        std::vector<MMCurrRequest> mmCurrRequests;
        std::string mmCurrGraphName;
        uint32_t agentGraphId;
        uint32_t workerId;
    };

    struct MMCurrAgentGraphs {
        std::vector<MMCurrAgentGraph> mmCurrAgentGraphs;
    };

    struct MMCurrWorkerThread {
        std::uint32_t workerId;
        std::uint64_t schedCount;
        std::string threadId;
        int cpuCore;
        std::string cpuUsage;
    };

    struct MMCurrWorkerThreads {
        std::vector<MMCurrWorkerThread> mmCurrWorkerThreads;
    };

    struct MMPrintDefaultResult {
        std::string msg;
    };

    struct MMAgentflowConfig {
        /**
         * 最大调度单次图外任务数
         */
        uint32_t maxSchedOnceTaskNum{1000};
        /**
         * 最大调度多次图外任务数
         */
        uint32_t maxSchedTaskNum{1000};
        /**
         * 最大当前请求任务队列数
         */
        uint32_t maxReqQueLen{1000};
        /**
         * 当前最大动态图数
         */
        uint32_t maxAgentGraphNum{1000};
        /**
         * 每个静态图最大Node数
         */
        uint32_t maxMMNodeNumPerGraph{100};
        /**
         * 最大静态图数
         */
        uint32_t maxMMGraphNum{100};
    };

    enum MMlogLevel {
        MM_LOG_DEBUG = 0,    /* 调试级别 */
        MM_LOG_INFO = 1,   /* 正常 */
        MM_LOG_NOTICE = 2, /* 不是错误，但是对定位问题比较关键的日志 */
        MM_LOG_WARN = 2,   /* 错误但不严重的信息 */
        MM_LOG_ERR = 3,    /* 严重需要立即处理的信息 */
        MM_LOG_FATAL = 4,  /* 致命的错误信息 */
    };

    class MMInnerAgentGraphConfig {
    public:
        static MMInnerAgentGraphConfig &GetInstance();

        void SetMMAgentflowConfig(MMAgentflowConfig mmAgentflowConfig);

        void SetMMAgentflowLogFunc(WriteLogFunc callFunction);

        // 触发输出
        MMCurrGraphs PrintCurMMGraphs();

        MMCurrAgentGraphs PrintCurMMAgentGraphs();

        MMCurrWorkerThreads PrintWorkerThread(uint32_t intervalTime);

        // 启动开关
        MMPrintDefaultResult PrintCurMMTasks();

        MMPrintDefaultResult PrintNodeUseTime();

        MMPrintDefaultResult PrintRequestUseTime();

        // 关闭开发
        MMPrintDefaultResult CancelPrintCurMMTasks();

        MMPrintDefaultResult CancelPrintNodeUseTime();

        MMPrintDefaultResult CancelPrintRequestUseTime();
    };
}
#endif // MMAGENTGRAPHCPP_MM_INNER_AGENT_GRAPH_CONFIG_H
