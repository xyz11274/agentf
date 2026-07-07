/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 */
//
// Created by l00909985 on 2025/11/18.
//

#ifndef SECFENCEMESSAGE_H
#define SECFENCEMESSAGE_H

#define FENCE_NO_RISK_SUCCESS 0x1000
#define FENCE_PARSE_ERROR 0x2000
#define FENCE_INTERNAL_ERROR 0x4000
#define EMPTY_FENCE_INSTANCE 0x8000
#define HTTP2_CLIENT_ERROR 0x10000

#define TYPE_FRONT_FENCE 0x0
#define TYPE_REAR_FENCE 0x1

#define FENCE_RESULT_ZERO 0x0
#define FENCE_RESULT_ONE 0x1

#define FENCE_START_STATUS 0x0
#define FENCE_MIDDLE_STATUS 0x1
#define FENCE_END_STATUS 0x2
#define FENCE_START_AND_END_STATUS 0x3

#define FENCE_NO_RISK_PASS 0x0
#define FENCE_RISK_PASS 0x10
#define FENCE_RISK_BLOCK 0x20

#define POLITICAL_RED_LINES "10000"
#define CULT_SUPERSTITION "20000"
#define SOCIAL_NEGATIVE "30000"
#define VULGAR_VIOLENCE "40000"
#define LIIEGAL_CRIME "50000"
#define DISCRIMINATION "60000"

#include "aisf_platform/utils/utils.h"
using namespace AisfPlat;


struct riskDetail {
    std::string riskLabel1;
    std::string evidence;
};

struct evidence {
    std::string detectionType;
    std::vector<riskDetail> details;
};

struct secFenceResp {
    std::string text;
    int result;
    std::vector<evidence> evidenceList;
};

struct secFenceRequest {
    std::string sessionId;
    std::string text;
    int requestType;
    int seqNumber;
    int businessId;
    int status;
    secFenceResp detectionResult;

    std::string toString() const
    {
        Json::Value root;
        root["sessionId"] = sessionId;
        root["text"] = text;
        root["requestType"] = requestType;
        root["seqNumber"] = seqNumber;
        root["businessId"] = businessId;
        root["status"] = status;
        return JsonParser::ToRawString(root);
    }
};

struct secFenceErr {
    std::string errCode;
    std::string errMsg;
};

#endif // SECFENCEMESSAGE_H
