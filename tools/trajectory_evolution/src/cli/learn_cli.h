// 学习命令：提交轨迹或训练评测证据，触发统一的后台学习流程。
// 导入时核验数据来源与集合用途，不把留出评测轨迹当作训练经验。

#pragma once

namespace trajectory::evolution {
// 处理训练轨迹导入命令，遵守来源、划分和幂等条件。
int run_learning_import(int argc, char **argv);
}
