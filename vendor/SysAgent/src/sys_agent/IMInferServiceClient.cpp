/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * 
 * IMInferServiceClient — 精简后只剩单例骨架。
 * 
 * 历史：
 *   旧协议（三段式 llm.task.create → stream → destroy + SyncInvokeComplete + 
 *   BuildRequestBody / ParseSingleSSEChunk 等）已全部删除。
 * 
 * 新协议（OpenAI 兼容格式透传）实现在 IMInferServiceClient.OpenAI.cpp 中。
 */

#include "sys_agent/IMInferServiceClient.h"

using namespace SYSAGENT;

namespace SysAgent {

// ==================== 静态成员定义 ====================
std::shared_ptr<IMInferServiceClient> IMInferServiceClient::instance_;
std::once_flag IMInferServiceClient::once_flag_;

// ==================== 构造函数 / 析构函数 ====================

IMInferServiceClient::IMInferServiceClient() {
}

IMInferServiceClient::~IMInferServiceClient() {
}

// ==================== 单例 ====================

std::shared_ptr<IMInferServiceClient> IMInferServiceClient::Instance()
{
    std::call_once(once_flag_, []() {
        instance_ = std::make_shared<IMInferServiceClient>();
    });
    return instance_;
}

} // namespace SysAgent
