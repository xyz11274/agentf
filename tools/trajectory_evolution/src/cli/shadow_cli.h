// 影子策略命令：读取经验快照，生成或检查经验注入建议。
// 本入口不把影子建议自动升级为已发布的运行控制。

#pragma once

namespace agentflow::trajectory {
// 处理只读策略相关命令，所有输入仍经过协议和隐私校验。
int run_shadow(int argc, char **argv);
} // 命名空间结束：agentflow::trajectory
