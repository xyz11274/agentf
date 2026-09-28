// 本地服务辅助约定：共享路径、安全读取、身份验证等基础能力。
// 同一规则供不同入口使用，避免 HTTP 与原生入口出现不同的权限语义。

#pragma once

#include "integration_json.h"

namespace trajectory::evolution::integration {
// 统一隐私检查不访问文件系统、网络或模型。
void validate_local_data(const Json &value);
}
