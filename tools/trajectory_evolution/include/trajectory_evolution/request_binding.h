// 请求绑定：把通用策略修改映射回宿主声明允许修改的输入字段。
// 绑定只作用于授权路径，不任意重写整个请求或扩大策略搜索空间。
// 无法应用的修改要保留原始业务输入和明确状态，不能静默伪造注入成功。
// 此处是对外声明，具体实现位于 src；修改接口时需考虑现有调用方与持久化版本。

#pragma once

#include "runtime_policy.pb.h"

namespace trajectory::evolution {
// 验证模型请求中可注入的位置和格式，防止写入错误字段破坏宿主协议。
void validate_request_binding(const v1::RuntimeOptions &options);
// 只在配置允许的请求位置注入经验，保留宿主原有消息和其他业务字段。
google::protobuf::Struct inject_request_context(const google::protobuf::Struct &request,
                                                const std::string &context,
                                                const v1::RuntimeOptions &options);
// 核对实际请求是否包含预期注入内容，用于区分生成建议和真正应用。
bool bound_request_contains(const google::protobuf::Struct &request, const std::string &context,
                            const v1::RuntimeOptions &options);
} // 命名空间结束：trajectory::evolution
