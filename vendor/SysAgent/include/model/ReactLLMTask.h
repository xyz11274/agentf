// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_REACT_LLMTASK_H
#define SYSAGENT_REACT_LLMTASK_H

#include "mm_customedreact_llm_component.h"
#include "mm_llm_component.h"
#include "utils/logger.h"
#include <functional>
#include <iostream>
#include <variant>

#define THINK_POSION 8

inline auto LLMomponent = std::make_shared<mmsdk::MMLlmComponent>();
inline auto reactLLMomponent = std::make_shared<mmsdk::MMCustomedReactLlmComponent>();
inline auto CustomedLLMComponent = std::make_shared<mmsdk::MMCustomedLlmComponentExt>();

enum class LLMEventType {
    TaskCreated,
    TaskDestroyed,
    OnLLMStart,
    OnLLMEnd,
    OnLLMNewToken,
    OnLLMFinalAnswer,
    OnLLMError,
    OnToolCallName,
    OnToolConfirm,
    OnToolStart,
    OnToolEnd,
    OnToolError
};

struct TaskCreatedEvent {
    uint32_t code;
    std::string taskId;
};

struct TaskDestroyedEvent {
    uint32_t code;
    std::string taskId;
};

struct OnLLMStartEvent {
    std::string taskId;
};

struct OnLLMEndEvent {};

struct OnLLMNewTokenEvent {};

struct OnLLMFinalAnswerEvent {
    std::string taskId;
    std::string reqId;
    std::string answer;
};

struct OnLLMErrorEvent {
    std::string taskId;
    std::string reqId;
    std::string errorMsg;
};

struct OnToolCallNameEvent {};

struct OnToolConfirmEvent {};

struct OnToolStartEvent {};

struct OnToolEndEvent {};

struct OnToolErrorEvent {};

using LLMEventData = std::variant<TaskCreatedEvent, TaskDestroyedEvent, OnLLMStartEvent, OnLLMEndEvent,
                                  OnLLMNewTokenEvent, OnLLMFinalAnswerEvent, OnLLMErrorEvent, OnToolCallNameEvent,
                                  OnToolConfirmEvent, OnToolStartEvent, OnToolEndEvent, OnToolErrorEvent>;
using EventCallback = std::function<void(const LLMEventData&)>;

struct ReactLLMTaskEvent {
    LLMEventType type;
    LLMEventData data;
};

class ReactLLMTask : public mmsdk::MMLLMAsyncCallBack, public std::enable_shared_from_this<ReactLLMTask> {
public:
    uint32_t OnLLMStart(const std::string& taskId, const std::string& reqId) override
    {
        LOG_INFO("[REACT_LLM] OnLLMStart taskId=%s reqId=%s", taskId.c_str(), reqId.c_str());
        if (callbacks_.find(LLMEventType::OnLLMStart) != callbacks_.end()) {
            callbacks_[LLMEventType::OnLLMStart](OnLLMStartEvent{});
        }
        return 0;
    }

    uint32_t OnLLMEnd(const std::string& taskId, const std::string& reqId) override
    {
        LOG_INFO("[REACT_LLM] OnLLMEnd taskId=%s reqId=%s", taskId.c_str(), reqId.c_str());
        if (callbacks_.find(LLMEventType::OnLLMEnd) != callbacks_.end()) {
            callbacks_[LLMEventType::OnLLMEnd](OnLLMEndEvent{});
        }
        return 0;
    }

    // 1、接收到大模型的返回结果
    uint32_t OnLLMNewToken(const std::string& taskId, const std::string& reqId, const std::string& token) override
    {
        LOG_DEBUG("[REACT_LLM] OnLLMNewToken taskId=%s reqId=%s token_len=%zu",
                  taskId.c_str(), reqId.c_str(), token.size());
        return 0;
    }

    // 结果汇总
    uint32_t OnLLMFinalAnswer(const std::string& taskId, const std::string& reqId, const std::string& answer) override
    {
        LOG_INFO("[REACT_LLM] OnLLMFinalAnswer taskId=%s reqId=%s answer_len=%zu",
                 taskId.c_str(), reqId.c_str(), answer.size());
        if (callbacks_.find(LLMEventType::OnLLMFinalAnswer) != callbacks_.end()) {
            callbacks_[LLMEventType::OnLLMFinalAnswer](OnLLMFinalAnswerEvent{taskId, reqId, answer});
        }
        return 0;
    }

    uint32_t OnLLMError(const std::string& taskId, const std::string& reqId, uint32_t resultCode) override
    {
        LOG_ERR("[REACT_LLM] OnLLMError taskId=%s reqId=%s resultCode=%u",
                taskId.c_str(), reqId.c_str(), resultCode);
        return 0;
    }

    uint32_t OnToolCallName(const std::string& taskId, const std::string& reqId, const std::string& toolName) override
    {
        LOG_INFO("[REACT_LLM] OnToolCallName taskId=%s reqId=%s toolName=%s",
                 taskId.c_str(), reqId.c_str(), toolName.c_str());
        return 0;
    }

    bool OnToolConfirm(const std::string& taskId, const std::string& reqId, const std::string& toolName,
                       const std::map<std::string, std::string>& toolArgs) override
    {
        return 0;
    }

    uint32_t OnToolStart(const std::string& taskId, const std::string& reqId, const std::string& toolName,
                         const std::map<std::string, std::string>& toolArgs) override
    {
        return 0;
    }

    uint32_t OnToolEnd(const std::string& taskId, const std::string& reqId, const std::string& toolName,
                       const std::map<std::string, std::string>& toolArgs, const std::string& toolResponse) override
    {
        return 0;
    }

    uint32_t OnToolError(const std::string& taskId, const std::string& reqId, const std::string& toolName,
                         const std::map<std::string, std::string>& toolArgs, const uint32_t resultCode) override
    {
        return 0;
    }

public:
    ReactLLMTask() {}
    ~ReactLLMTask() {}

    void RegisterCallback(LLMEventType type, EventCallback callback) { callbacks_[type] = std::move(callback); }

private:
    std::map<LLMEventType, EventCallback> callbacks_;
};

inline std::string trimThinkFlag(std::string answer)
{
    size_t think_pos = answer.find("</think>");
    std::string result = "";
    if (think_pos == std::string::npos) {
        result = answer;
    } else {
        result = answer.substr(think_pos + THINK_POSION, answer.size());
    }
    return result;
}

#endif  // SYSAGENT_REACT_LLMTASK_H