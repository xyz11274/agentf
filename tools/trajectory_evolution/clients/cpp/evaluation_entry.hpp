// cpp 客户端：通过统一协议收集执行事件并接收策略建议。
// 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

#pragma once
#include <trajectory_evolution/evaluation_entry.h>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace trajectory_evolution_http {
// 传入现有 JSON 接收函数即可，不需要框架专用启动器。
template <class Receiver> bool evaluate(Receiver &&receiver) {
  auto bound = [&receiver](const std::string &input) { receiver(input); };
  using Function = decltype(bound);
  const auto call = [](const char *input, void *context) -> int {
    try {
      (*static_cast<Function *>(context))(std::string(input));
      return 0;
    } catch (...) {
      return -1;
    }
  };
  const auto result = te_evaluate(call, &bound);
  if (result < 0)
    throw std::runtime_error("evaluation_entry_failed");
  return result == 1;
}
} // 命名空间结束：trajectory_evolution_http
