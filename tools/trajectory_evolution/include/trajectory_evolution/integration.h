// 公开接口：定义宿主接入所需的类型、配置和调用约定。
// 此处是对外声明，具体实现位于 src；修改接口时需考虑现有调用方与持久化版本。

#pragma once

#include <filesystem>
#include <cstdint>
#include <memory>
#include <map>
#include <string>

namespace trajectory::evolution {

struct IntegrationResponse {
  int status = 200;
  std::string body;
};

// 语言无关的事件与控制接口，HTTP 和进程内入口共用实现。
// 宿主执行始终由宿主负责，不委托给本组件。
class IntegrationService {
public:
  explicit IntegrationService(const std::filesystem::path &config,
                              const std::map<std::string, std::string> &environment = {});
  IntegrationService(const std::filesystem::path &config,
                     const std::map<std::string, std::string> &environment,
                     bool enable_auto_evaluation);
  // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
  ~IntegrationService();
  IntegrationService(const IntegrationService &) = delete;
  IntegrationService &operator=(const IntegrationService &) = delete;
  // 包装宿主业务调用：先查询策略，再最多执行一次原业务函数，最后记录结果。
  IntegrationResponse call(const std::string &bearer, const std::string &request);
  // 这是可信本地导入，不是允许 HTTP 读取路径的接口。
  // 已完成历史日志与在线运行进入同一持久队列；完全重复导入不产生新工作。
  IntegrationResponse import_file(const std::filesystem::path &path);
  // 显式冷启动准入：只接收已验证且未修改的 L0 训练试验。
  IntegrationResponse import_training_evaluation(const std::filesystem::path &report_directory,
                                                 const std::filesystem::path &dataset_directory);
  // 返回后台学习队列和处理状态，不触发额外模型请求。
  std::string learning_status() const;
  // 在后台处理已到期的评测工作，遵守批次、频率和取消约束。
  void process_evaluations();
  // 显式维护可刷新小批次，后台处理仍遵守频率限制。
  void process_evaluations(bool flush);
  // 离线候选评测专用，不处理轨迹学习、私库整理或公共库审查。
  void process_agent_evaluations();
  // 读取后台工作版本，供通知与等待避免频繁空闲刷新。
  std::uint64_t work_version() const;
  // 等待工作版本变化或关闭通知，避免持续空转扫描。
  void wait_for_work(std::uint64_t observed, unsigned max_wait_ms);
  // 唤醒等待中的后台线程，主调用方不等待实际学习完成。
  void wake_background();
  // 返回监听端口或配置端口，不触发新服务启动。
  unsigned port() const;
  // 返回 HTTP 请求总期限配置，不在重试时重新扩大预算。
  unsigned http_timeout_ms() const;
  // 返回服务工作线程配置，供启动和容量管理使用。
  unsigned workers() const;
  // 返回单请求大小上限，避免热路径无界编码。
  std::size_t max_request_bytes() const;
  // 仅供维护使用：等待已接受的缓冲采集，不等待学习完成。
  bool flush_capture(unsigned timeout_ms = 5000);
  // 检查指定运行的采集是否真正结束，不能仅看派发进程是否退出。
  bool capture_finished() const;
  // 检查指定运行的采集是否真正结束，不能仅看派发进程是否退出。
  bool capture_finished(std::size_t expected_runs) const;
  // 读取隔离评测交付入口状态，区分等待、运行和完成。
  std::string evaluation_entry_status() const;

private:
  friend class LocalRuntime;
  // 识别并校验导入来源，将有效日志送入与在线采集相同的后台队列。
  IntegrationResponse import_source(const std::filesystem::path &path,
                                    const std::string &training_trace,
                                    const std::string &training_source);
  // 导入已验证训练报告的各条真实轨迹，按来源身份避免重复学习。
  IntegrationResponse import_training_report(const std::filesystem::path &report_directory,
                                             const std::filesystem::path &dataset_directory,
                                             const std::string &baseline_json);
  // 在后台或管理请求中同步已发布快照；主 Agent 打点只读缓存。
  void refresh_published_versions(const std::string &user);
  // 在后台刷新候选评测与发布状态，业务打点不进入阻塞评测路径。
  void refresh_agent_evaluations();
  // 仅供启动或后台桥接使用，不向 HTTP 调度器开放。
  std::string local_environment(const std::string &bearer, std::uint64_t memory_version = 0);
  // 持久化本地已接受状态，后台写入与业务运行锁分离。
  IntegrationResponse persist_local(const std::string &bearer, const std::string &state);
  // 把本地运行的可恢复状态交给后台持久化，保证采集缺口同步保存。
  void checkpoint_local(const std::string &bearer, const std::string &instance,
                        const std::string &metrics);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// 持有回环监听器和后台线程，可供隔离评测器使用。
class IntegrationHttpServer {
public:
  explicit IntegrationHttpServer(const std::filesystem::path &config,
                                 const std::map<std::string, std::string> &environment = {});
  IntegrationHttpServer(const std::filesystem::path &config,
                        const std::map<std::string, std::string> &environment,
                        bool enable_auto_evaluation);
  // 停止监听并收尾后台线程，之后不再接收新的 HTTP 请求。
  ~IntegrationHttpServer();
  // 返回监听端口或配置端口，不触发新服务启动。
  unsigned port() const;
  // 等待已接受的采集缓冲完成持久化；不等待模型学习或策略评测。
  bool flush_capture(unsigned timeout_ms = 5000);
  // 检查指定运行的采集是否真正结束，不能仅看派发进程是否退出。
  bool capture_finished() const;
  // 检查指定运行的采集是否真正结束，不能仅看派发进程是否退出。
  bool capture_finished(std::size_t expected_runs) const;
  // 读取隔离评测交付入口状态，区分等待、运行和完成。
  std::string evaluation_entry_status() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// 启动统一协议服务及后台处理器，关闭时先停止新请求再回收资源。
int run_integration_server(int argc, char **argv);

} // 命名空间结束：trajectory::evolution
