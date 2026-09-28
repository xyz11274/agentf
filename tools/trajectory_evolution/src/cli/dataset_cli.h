// 数据集命令：导入、检查和冻结任务集合，保持来源与 split 可追溯。
// 这里只编排数据集 API，不启动宿主或擅自运行模型评测。

#pragma once

namespace trajectory::evolution {
// 处理数据集命令行入口，将参数校验与数据构建实现分离。
int run_dataset(int argc, char **argv);
// 解析评测命令并安装取消处理，随后委托统一执行器。
int run_task_evaluation_cli(int argc, char **argv);
} // 命名空间结束：trajectory::evolution
