#pragma once

#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "message/ServerEntity.h"

struct QueueMessage {
    std::shared_ptr<SYSAGENT::AccessCtrl> data;
    PerSession *session;
};

