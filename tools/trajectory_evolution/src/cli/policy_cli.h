// 策略命令：处理版本编译、导入导出和可复用策略包。
// 策略版本与本次运行身份分开；导出不能携带凭据或依赖临时工作目录。

#pragma once

namespace agentflow::trajectory {
// 处理策略编译、导出或查询命令，不把命令行解析嵌入运行算法。
int run_policy_runtime(int argc, char **argv);
}
