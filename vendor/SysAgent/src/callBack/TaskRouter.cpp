//
// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
//

#include "callBack/TaskRouter.h"
#include "utils/logger.h"

using namespace SYSAGENT;
using namespace CMFrm::ServiceRouter;

std::string TaskRouter::GetAPIProvider()
{
    return "SysAgent";
}

std::vector<UsrServiceRouter> TaskRouter::URLPatterns()
{
    std::shared_lock<std::shared_mutex> guard(lock);
    return routerList;
}

bool TaskRouter::Register(const UsrServiceRouter& router)
{
    std::unique_lock<std::shared_mutex> guard(lock);
    routerList.emplace_back(router);
    return true;
}