//
// Created by c00888021 on 2025/8/25.
//

#ifndef SYSAGENT_LIFECYCLEHANDLER_H
#define SYSAGENT_LIFECYCLEHANDLER_H

#ifdef Expand
#undef Expand
#endif
#include "aisf_platform/parser/aisf_msg_ctrl.h"

class LifecycleHandler {
public:
    virtual ~LifecycleHandler() = default;

    virtual void handle(const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl) = 0;
};

class LifecycleHandlerManager {
public:
    static LifecycleHandlerManager& GetInstance()
    {
        static LifecycleHandlerManager instance;
        return instance;
    }
    void Init();
    std::unordered_map<SYSAGENT::EventType, std::shared_ptr<LifecycleHandler>>& getHandlerList()
    {
        return GetInstance().lifecycleHandlerList;
    }
    void addHandlerList(SYSAGENT::EventType eventType, const std::shared_ptr<LifecycleHandler>& handler)
    {
        GetInstance().lifecycleHandlerList[eventType] = handler;
    };
private:
    std::unordered_map<SYSAGENT::EventType, std::shared_ptr<LifecycleHandler>> lifecycleHandlerList;
};
#endif // SYSAGENT_LIFECYCLEHANDLER_H
