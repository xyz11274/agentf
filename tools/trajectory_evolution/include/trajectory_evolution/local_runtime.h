// 进程内低延迟运行时：读取固定策略并将采集事件交给后台处理。
// 主 Agent 的同步路径不能等待模型评测或全库管理；队列和负载保持有界。
// 关闭、排空和失败降级分别处理，不能为了采集阻断业务执行。
// 此处是对外声明，具体实现位于 src；修改接口时需考虑现有调用方与持久化版本。

#pragma once

#include "trajectory_evolution/integration.h"
#include <cstddef>
#include <memory>

namespace trajectory::evolution {

struct LocalRuntimeOptions {
  std::size_t max_request_bytes = 65536;
  std::size_t max_buffer_bytes = 67108864;
  std::size_t max_pending_runs = 64;
  unsigned poll_interval_ms = 20;
  bool background = true;
  unsigned contention_budget_us = 1000;
};

// 业务请求到来前创建一次；invoke 只进行有界本地计算，
// 不访问套接字、文件系统或评测器；单次运行锁竞争预算较小且可配置。
// 后台 I/O 不持有业务运行锁。
// 繁忙、容量不足或失败应明确标记降级，不能视为成功观测。
class LocalRuntime {
public:
  LocalRuntime(const std::filesystem::path &config, const std::string &bearer,
               LocalRuntimeOptions options = {});
  // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
  ~LocalRuntime();
  LocalRuntime(const LocalRuntime &) = delete;
  LocalRuntime &operator=(const LocalRuntime &) = delete;
  // 执行一次组件协议调用，检查响应信封并保持请求身份一致。
  IntegrationResponse invoke(const std::string &request) noexcept;
  // 读取组件自身的采集和排队指标，不把观测指标当作业务任务得分。
  std::string metrics() const;
  // 暂停只影响进化附加能力，不修改宿主自身的安全约束。
  // 恢复只对新运行生效，中途被打断的运行仍保持隔离。
  void set_enabled(bool enabled) noexcept;
  // 这些操作可能阻塞，仅用于维护、测试或关闭，不用于业务边界。
  IntegrationResponse manage(const std::string &request) noexcept;
  // 等待当前待持久化工作完成，用于维护或关闭；不要放在主 Agent 每个调用的热路径。
  void drain();
  // 显式处理待评测工作，只用于后台或维护调用，不阻塞主 Agent 边界。
  void evaluate_pending();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // 命名空间结束：trajectory::evolution
