/* ******************************************************************************
 *
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 *
 * -------------------------------------------------------------------------------
 *
 * @file logger.h
 *
 * Project Code: SysAgent
 * Module Name: utils
 * Date Created: 2025-05-27
 * Author:
 * Description: Definitions of Logger
 *
 * -------------------------------------------------------------------------------
 * Modification History
 * DATE          NAME                       DESCRIPTION
 * -----------------------------------------------------------------------------
 *
 * ***************************************************************************** */

#ifndef SYSAGENT_LOGGER_H
#define SYSAGENT_LOGGER_H

#include "mmagentflow/mm_sdk.h"
#include "framework_common/logger/logger.h"

namespace SysAgent {
#define LOG_STDOUT(format, args...) CMFrm_Stdout(format, ##args)
#define LOG_STDERR(format, args...) CMFrm_Stderr(format, ##args)
#define LOG_DEBUG(format, args...) CMFrm_Debug(format, ##args)
#define LOG_INFO(format, args...) CMFrm_Info(format, ##args)
#define LOG_INFO_EX(format, args...) CMFrm_Info(format, ##args)
#define LOG_WARN(format, args...) CMFrm_Warning(format, ##args)
#define LOG_ERR(format, args...) CMFrm_Error(format, ##args)
#define LOG_ERR_EX(format, args...) CMFrm_Error(format, ##args)
#define LOG_INFO_SAFE(format, args...) CMFrm_Info(format, ##args)
#define LOG_ERR_SAFE(format, args...) CMFrm_Error(format, ##args)
#define LOG_FATAL(format, args...) CMFrm_Fatal(format, ##args)
#define PRINT_AND_LOG_INFO(format, args...) LOG_INFO(format, ##args); LOG_STDOUT(format, ##args)
#define PRINT_AND_LOG_ERR(format, args...) LOG_ERR(format, ##args); LOG_STDOUT(format, ##args)
}  // namespace SysAgent

#endif  // SYSAGENT_LOGGER_H
