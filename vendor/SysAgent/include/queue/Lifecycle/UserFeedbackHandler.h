//
// Created by l00889076 on 2026/1/5.
//

#ifndef SYSAGENT_USERFEEDBACK_H
#define SYSAGENT_USERFEEDBACK_H

#ifdef Expand
#undef Expand
#endif
#include "queue/Lifecycle/LifecycleHandler.h"

class UserFeedbackHandler : public LifecycleHandler {
public:
    void handle(const shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl) override;
};

#endif //SYSAGENT_USERFEEDBACK_H
