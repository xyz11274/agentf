// probe 回归验证：检查包内协议、边界条件与失败处理。
// 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
// 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

#include "../../clients/cpp/evolution.hpp"
#include <cstdlib>
#include <iostream>

// 回归用例：main。
// 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
int main() {
  if (te_initialize() != 0)
    return 2;
  trajectory_evolution_http::Client client(std::getenv("EVOLUTION_URL"),
                                           std::getenv("EVOLUTION_TEST_TOKEN"), 7000, 2097152, 0);
  for (std::string line; std::getline(std::cin, line);) {
    try {
      std::cout << client.invoke(line) << std::endl;
    } catch (const trajectory_evolution_http::Error &error) {
      std::cout << "{\"ok\":false,\"error\":\"" << error.what() << "\",\"status\":" << error.status
                << "}" << std::endl;
    }
  }
  te_cleanup();
  return 0;
}
