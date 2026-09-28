// 轨迹命令：读取标准日志并调用验证、分析和评测接口。
// 解析错误应保留文件与结构诊断，不能通过忽略问题事件得到虚假的完整轨迹。

#pragma once

#include <filesystem>

namespace agentflow::trajectory {
// 识别是否为标准轨迹输入，决定使用通用解析还是兼容入口。
bool is_standard_trace_input(const std::filesystem::path &path);
// 检查输入轨迹的格式、完整性和可用能力，不凭缺失字段猜测支持等级。
int run_doctor(int argc, char **argv);
} // 命名空间结束：agentflow::trajectory
