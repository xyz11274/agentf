// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_BASENODE_H
#define SYSAGENT_BASENODE_H

#include "mm_message.h"
#include "mm_message_factory.h"
#include "mm_node.h"
#include <memory>
#include "framework_common/timermanager/timer_manager_model.h"
#include "framework_common/timermanager/timer_manager.h"

using ConditionFunc =
    std::function<std::vector<std::shared_ptr<mmagentflow::MMNode>>(std::shared_ptr<mmagentflow::MMMessage> msg)>;

template <typename T> class BaseData : public mmagentflow::MMData {
public:
    BaseData(T value) : value_(value)
    {
    }
    std::shared_ptr<mmagentflow::MMData> DeepCopy() override
    {
        return std::make_shared<BaseData>(value_);
    };
    uint32_t DataLength() override
    {
        return 0;
    };
    void setValue(const T& value)
    {
        value_ = value;
    }

    T& getValue()
    {
        return value_;
    }

private:
    T value_;
};

class BaseNode : public mmagentflow::MMNode, public std::enable_shared_from_this<BaseNode> {
public:
    BaseNode(const std::string& nodeName, const mmagentflow::MMNodeConfig& nodeConfig)
        : name_(nodeName), config_(nodeConfig)
    {
    }

    /**
     * 平台获取节点配置
     */
    mmagentflow::MMNodeConfig& GetNodeConfig() override
    {
        return config_;
    }

    /**
     * 获取节点名称
     * @return
     */
    std::string& GetName() override
    {
        return name_;
    }

    mmagentflow::MMNodeFunctionStatus ErrorRun(std::shared_ptr<mmagentflow::MMMessage> msg, uint32_t errcode) override;

    static void SetTimeoutTask(std::shared_ptr<mmagentflow::MMMessage> msg, const std::string& timerKey,
                               int timeoutSeconds, std::function<void()> timeoutCallback);
protected:
    std::string name_;
    mmagentflow::MMNodeConfig config_;
};

#endif // SYSAGENT_BASENODE_H