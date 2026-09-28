// cpp 客户端：通过统一协议收集执行事件并接收策略建议。
// 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

#pragma once
#include "../c/evolution.h"
#include <memory>
#include <stdexcept>
#include <string>

namespace trajectory_evolution_http {
struct Error : std::runtime_error {
  long status;

  // 创建或读取受限错误信息，保留固定错误码而不泄露业务正文。
  Error(const char *code, long value) : std::runtime_error(code), status(value) {}
};

// 不依赖模型 SDK 或特定 JSON 库；网络资源通过 libcurl 管理。
// 在线程启动前调用一次 te_initialize()，然后由调用方解析返回信封。
class Client {
public:
  // 初始化 Client 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  Client(std::string url, std::string token, long timeout_ms = 7000,
         std::size_t max_response_bytes = 2097152, unsigned retries = 1)
      : url_(std::move(url)), token_(std::move(token)), timeout_(timeout_ms),
        limit_(max_response_bytes), retries_(retries) {}

  // 执行一次组件协议调用，检查响应信封并保持请求身份一致。
  std::string invoke(const std::string &request_json) const {
    te_client client{url_.c_str(), token_.c_str(), timeout_, limit_, retries_};
    char *result = nullptr;
    const char *code = nullptr;
    long status = 0;
    if (te_invoke(&client, request_json.c_str(), &result, &status, &code) != 0)
      throw Error(code, status);
    std::unique_ptr<char, decltype(&te_free)> buffer(result, &te_free);
    return std::string(buffer.get());
  }

private:
  std::string url_, token_;
  long timeout_;
  std::size_t limit_;
  unsigned retries_;
};
} // 命名空间结束：trajectory_evolution_http
