/* ******************************************************************************
 *
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 *
 * -------------------------------------------------------------------------------
 *
 * @file router.h
 *
 * Project Code: SysAgent
 * Module Name: Task
 * Date Created: 2025-05-27
 * Author:
 * Description: Definitions of Task Router
 *
 * -------------------------------------------------------------------------------
 * Modification History
 * DATE          NAME                       DESCRIPTION
 * -----------------------------------------------------------------------------
 *
 * ***************************************************************************** */

#ifndef SYSAGENT_ROUTER_H
#define SYSAGENT_ROUTER_H

#include <shared_mutex>

#include "framework_om/service_router/router_manager.h"

namespace SYSAGENT {

class TaskRouter final : public CMFrm::ServiceRouter::APIRouter {
public:
    std::string GetAPIProvider() override;

    std::vector<CMFrm::ServiceRouter::UsrServiceRouter> URLPatterns() override;

    bool Register(const CMFrm::ServiceRouter::UsrServiceRouter& router);

private:
    std::shared_mutex lock;

    std::vector<CMFrm::ServiceRouter::UsrServiceRouter> routerList;
};
}  // namespace SYSAGENT

#endif  // SYSAGENT_ROUTER_H
