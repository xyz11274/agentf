//
// Created by c00888021 on 2025/8/25.
//

#ifndef SYSAGENT_LOCATIONHANDLER_H
#define SYSAGENT_LOCATIONHANDLER_H

#ifdef Expand
#undef Expand
#endif
#include "queue/Lifecycle/LifecycleHandler.h"
#include "queue/SystemMessageReadQueueHandler.h"
#include "TeHelper.h"
#include "mm_sdk.h"
#include "data_source.h"
#include "memory/Location.h"

class LocationHandler : public LifecycleHandler {
public:
    void handle(const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl) override;
};

#endif // SYSAGENT_LOCATIONHANDLER_H
