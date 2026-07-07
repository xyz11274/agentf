/*
* Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
//
// Created by l00909985 on 2025/11/14.
//

#ifndef SECURITYFENCE_H
#define SECURITYFENCE_H

#include "utils/http_client.h"
#include "aisf_platform/utils/utils.h"
#include "SecFenceMessage.h"
#include "taskManage/TeamEnvironment.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "framework_om/service_router/router_manager.h"
#include <vector>

using namespace AisfPlat;

#define FENCE_REQ_TIMEOUT 2

constexpr std::string_view security_fence_url = "/rest/genmoguardservice/v1/text";
constexpr std::string_view security_fence_workload_url = "/rest/genmoguardservice/v1/workload";
constexpr std::string_view security_fence_healthcheck_url = "/rest/genmoguardservice/v1/healthcheck";

int getFrontFenceDetection(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl);
int getRearFenceDetection(const std::shared_ptr<SYSAGENT::AccessCtrl>& accessCtrl);

bool ParseSecFenceResp(const std::string& secFenceRespStr, secFenceResp& resp);
int checkFenceResp(const secFenceResp& resp);
std::string getSecFenceServiceEndPoint();
std::string getRejectMsgZh();
std::string getRejectMsgEn();

void initRandomSeed();
unsigned int generateBusinessId();
void OnRecvSecurityStatusRest(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

#endif // SECURITYFENCE_H
