//
// Created by c00888021 on 2025/8/25.
//
#include "queue/Lifecycle/LifecycleHandler.h"
#include "queue/Lifecycle/LocationHandler.h"
#include "queue/Lifecycle/UserPortraitConfHandler.h"

void LifecycleHandlerManager::Init()
{
    LifecycleHandlerManager::addHandlerList(SYSAGENT::EventType::LOCATION,
                                            std::make_shared<LocationHandler>());
    LifecycleHandlerManager::addHandlerList(SYSAGENT::EventType::USERPORTRAITCONF,
                                            std::make_shared<UserPortraitConfHandler>());
}