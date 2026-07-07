// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_RATELIMITRULEHANDLER_H
#define SYSAGENT_RATELIMITRULEHANDLER_H

#include "AbstractRuleHandler.h"
using namespace SYSAGENT;
class RateLimitRuleHandler : public AbstractRuleHandler {
public:
    shared_ptr<Rule> doChain(const std::shared_ptr<AccessCtrl>& accessCtrl) override;
};

#endif //SYSAGENT_RATELIMITRULEHANDLER_H
