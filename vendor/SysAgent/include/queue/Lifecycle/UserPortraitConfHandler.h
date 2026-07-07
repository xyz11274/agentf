//
// Created by c00888021 on 2025/8/25.
//

#ifndef SYSAGENT_USERMESAGEHANDLER_H
#define SYSAGENT_USERMESAGEHANDLER_H

#ifdef Expand
#undef Expand
#endif
#include "queue/Lifecycle/LifecycleHandler.h"

class UserPortraitConfHandler : public LifecycleHandler {
public:
    void handle(const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl) override;
};

#endif // SYSAGENT_USERMESAGEHANDLER_H
