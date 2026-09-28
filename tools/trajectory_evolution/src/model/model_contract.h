// 模型输出契约：把文本响应解析为结构化评测或反思结果并逐项校验。
// 模型输出是不可信输入，不能绕过字段、枚举、证据引用和长度限制。
// 解析失败返回可诊断状态，不用默认通过或空结果掩盖错误。

#pragma once

#include "integration_json.h"

namespace trajectory::evolution::integration {

// 只读取线缆格式元数据，不读取描述、模型正文或应用源码来猜测能力。
Json infer_model_binding(const Json &input);
// 从真实返回结构提取 token 使用量，区分新输入和缓存消耗。
Json infer_model_usage(const Json &output);
// 从工具声明提取可执行动作契约，不能从历史基线猜测允许修改的动作。
Json model_tool_contracts(const Json &input);
// 核对工具调用是否符合已声明的参数契约，拒绝不支持的名称与结构。
Json check_tool_contract(const Json &contracts, const std::string &operation,
                         const Json &arguments);

} // 命名空间结束：trajectory::evolution::integration
