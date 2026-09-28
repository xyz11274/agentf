// HTTP 传输入口：把语言无关 JSON 请求交给统一接入服务。
// HTTP 是进程间调用方式，不要求 Agent 本身是 Web 应用。
// 身份认证、请求大小和错误响应在边界处理，业务输出与诊断信息不得混淆。

#include "trajectory_evolution/integration.h"
#include "integration_json.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace trajectory::evolution {
namespace {
using namespace integration;
using Clock = std::chrono::steady_clock;
static_assert(std::atomic<bool>::is_always_lock_free, "signal flag must be lock-free");
std::atomic<bool> stopping{false};

// 设置服务停止标记，主循环在安全边界停止接收并完成收尾。
void stop_server(int) {
  stopping = 1;
}

struct Socket {
  int fd;

  // 初始化 Socket 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  explicit Socket(int value) : fd(value) {}

  // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
  ~Socket() {
    if (fd >= 0)
      ::close(fd);
  }
};

// 在截止期限内等待套接字可读写，处理取消与错误状态。
bool ready(int fd, short events, Clock::time_point deadline, const std::atomic<bool> &stopped) {
  while (!stopped) {
    auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (remaining <= 0)
      return false;
    pollfd p{fd, events, 0};
    const auto result = ::poll(&p, 1, std::min<std::int64_t>(remaining, 200));
    if (result > 0)
      return p.revents & events;
    if (result < 0 && errno != EINTR)
      return false;
  }
  return false;
}

// 发送受限标准响应，错误只返回固定码，不泄露请求正文。
void send_response(int fd, const IntegrationResponse &response, Clock::time_point deadline,
                   const std::atomic<bool> &stopped) {
  const auto data =
      "HTTP/1.1 " + std::to_string(response.status) +
      " Response\r\n"
      "Content-Type: application/json; charset=utf-8\r\nConnection: close\r\n"
      "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Length: " +
      std::to_string(response.body.size()) + "\r\n\r\n" + response.body;
  std::size_t sent = 0;
  while (sent < data.size() && ready(fd, POLLOUT, deadline, stopped)) {
    auto n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n < 0 && (errno == EINTR || errno == EAGAIN))
      continue;
    if (n <= 0)
      return;
    sent += n;
  }
}

// 处理一次连接的请求解析、认证、路由和响应，限制时间与大小。
void connection(int fd, IntegrationService &service, const std::atomic<bool> &stopped) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(service.http_timeout_ms());
  IntegrationResponse response;
  try {
    std::string bytes;
    char buffer[8192];
    std::size_t split;
    auto receive = [&] {
      require(ready(fd, POLLIN, deadline, stopped), "request_timeout", 408);
      auto n = ::recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
      require(n > 0, "request_incomplete");
      bytes.append(buffer, n);
    };
    while ((split = bytes.find("\r\n\r\n")) == std::string::npos) {
      require(bytes.size() < 16384, "headers_too_large", 431);
      receive();
    }
    require(split <= 16384, "headers_too_large", 431);
    const auto first = bytes.find("\r\n");
    const auto line = bytes.substr(0, first);
    const bool health = line == "GET /v1/health HTTP/1.1";
    require(health || line == "POST /v1/invoke HTTP/1.1", "route_not_found", 404);
    std::map<std::string, std::string> headers;
    std::size_t begin = first + 2;
    while (begin < split) {
      auto end = bytes.find("\r\n", begin), colon = bytes.find(':', begin);
      require(colon > begin && colon < end, "header_invalid");
      auto name = bytes.substr(begin, colon - begin);
      for (unsigned char c : name)
        require(std::isalnum(c) || c == '-', "header_invalid");
      std::transform(name.begin(), name.end(), name.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      auto value = bytes.substr(colon + 1, end - colon - 1);
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.erase(value.begin());
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.pop_back();
      for (unsigned char c : value)
        require(c >= 32 && c != 127, "header_invalid");
      require(headers.emplace(name, value).second, "duplicate_header");
      begin = end + 2;
    }
    require(!headers.count("transfer-encoding") && !headers.count("expect"),
            "unsupported_transfer");
    require(headers.count("host"), "host_required");
    if (health) {
      require(!headers.count("content-length") || headers.at("content-length") == "0",
              "health_body_not_allowed");
      response = {200, R"({"ok":true,"protocol":"trajectory-integration.v1","max_level":"L3"})"};
    } else {
      require(headers.count("content-length"), "length_required", 411);
      const auto length = headers.at("content-length");
      require(!length.empty() && length.size() <= 8 &&
                  length.find_first_not_of("0123456789") == std::string::npos,
              "length_invalid");
      const auto size = std::stoull(length);
      require(size > 0 && size <= service.max_request_bytes(), "request_too_large", 413);
      const auto type = headers["content-type"];
      require(type == "application/json" || type == "application/json; charset=utf-8",
              "json_content_type_required", 415);
      const auto auth = headers["authorization"];
      require(auth.compare(0, 7, "Bearer ") == 0 && auth.size() > 7, "unauthorized", 401);
      while (bytes.size() < split + 4 + size)
        receive();
      require(bytes.size() == split + 4 + size, "pipelining_not_supported");
      response = service.call(auth.substr(7), bytes.substr(split + 4, size));
    }
  } catch (const Error &error) {
    response = {error.status, Json{{"ok", false}, {"error", error.what()}}.dump()};
  } catch (...) {
    response = {500, R"({"ok":false,"error":"transport_internal_error"})"};
  }
  // 读取期限到达后，允许短暂且有界地写出错误响应。
  send_response(fd, response, std::max(deadline, Clock::now() + std::chrono::milliseconds(100)),
                stopped);
}
} // 结束当前匿名命名空间。

struct IntegrationHttpServer::Impl {
  IntegrationService service;
  Socket listener;
  std::atomic<bool> stopped{false};
  std::vector<std::thread> workers;
  std::thread background;
  unsigned bound_port = 0;

  // 初始化 Impl 的状态和资源；输入参数的校验与所有权在构造阶段确定。
  // 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
  Impl(const std::filesystem::path &path, const std::map<std::string, std::string> &environment,
       bool enable_auto_evaluation)
      : service(path, environment, enable_auto_evaluation),
        listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) {
    require(listener.fd >= 0, "listen_failed", 500);
    int reuse = 1;
    ::setsockopt(listener.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(service.port());
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    require(::bind(listener.fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0 &&
                ::listen(listener.fd, 64) == 0,
            "listen_failed", 500);
    socklen_t length = sizeof(address);
    require(::getsockname(listener.fd, reinterpret_cast<sockaddr *>(&address), &length) == 0,
            "listen_failed", 500);
    bound_port = ntohs(address.sin_port);
    try {
      for (unsigned i = 0; i < service.workers(); ++i)
        workers.emplace_back([this] {
          while (!stopped) {
            if (!ready(listener.fd, POLLIN, Clock::now() + std::chrono::milliseconds(200), stopped))
              continue;
            Socket client(::accept4(listener.fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
            if (client.fd >= 0)
              connection(client.fd, service, stopped);
          }
        });
      background = std::thread([this] {
        while (!stopped) {
          const auto version = service.work_version();
          try {
            service.process_evaluations();
          } catch (...) {
            std::cerr << "integration: background storage/evaluation unavailable\n";
          }
          if (!stopped)
            service.wait_for_work(version, 1000);
        }
      });
    } catch (...) {
      stop();
      throw;
    }
  }

  // 请求停止当前工作并完成资源收尾；不将停止信号解释为业务成功。
  void stop() {
    stopped = true;
    service.wake_background();
    for (auto &worker : workers)
      if (worker.joinable())
        worker.join();
    if (background.joinable())
      background.join();
  }

  // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
  ~Impl() {
    stop();
  }
};

// 初始化 IntegrationHttpServer 的状态和资源；输入参数的校验与所有权在构造阶段确定。
// 对象创建后复用既有配置，实际执行与收尾分别由后续接口负责。
IntegrationHttpServer::IntegrationHttpServer(const std::filesystem::path &path,
                                             const std::map<std::string, std::string> &environment)
    : IntegrationHttpServer(path, environment, true) {}

IntegrationHttpServer::IntegrationHttpServer(const std::filesystem::path &path,
                                             const std::map<std::string, std::string> &environment,
                                             bool enable_auto_evaluation)
    : impl_(std::make_unique<Impl>(path, environment, enable_auto_evaluation)) {}

IntegrationHttpServer::~IntegrationHttpServer() = default;

// 返回监听端口或配置端口，不触发新服务启动。
unsigned IntegrationHttpServer::port() const {
  return impl_->bound_port;
}

// 等待已接受的采集缓冲完成持久化；不等待模型学习或策略评测。
bool IntegrationHttpServer::flush_capture(unsigned timeout_ms) {
  return impl_->service.flush_capture(timeout_ms);
}

// 检查指定运行的采集是否真正结束，不能仅看派发进程是否退出。
bool IntegrationHttpServer::capture_finished() const {
  return impl_->service.capture_finished();
}

// 检查指定运行的采集是否真正结束，不能仅看派发进程是否退出。
bool IntegrationHttpServer::capture_finished(std::size_t expected_runs) const {
  return impl_->service.capture_finished(expected_runs);
}

// 读取隔离评测交付入口状态，区分等待、运行和完成。
std::string IntegrationHttpServer::evaluation_entry_status() const {
  return impl_->service.evaluation_entry_status();
}

// 启动统一协议服务及后台处理器，关闭时先停止新请求再回收资源。
int run_integration_server(int argc, char **argv) {
  if (argc == 3 && std::string(argv[2]) == "--help") {
    std::cout << "trajectory_evolution serve --config <service.json> [--no-auto-evaluate]\n"
                 "Loopback HTTP: POST /v1/invoke; Bearer credential from configured environment.\n";
    return 0;
  }
  try {
    require((argc == 4 || argc == 5) && std::string(argv[2]) == "--config" &&
                (argc == 4 || std::string(argv[4]) == "--no-auto-evaluate"),
            "usage: serve --config <service.json> [--no-auto-evaluate]");
    stopping = 0;
    const auto previous_term = std::signal(SIGTERM, stop_server),
               previous_int = std::signal(SIGINT, stop_server);

    struct WorkerCleanup {
      void (*term)(int);
      void (*interrupt)(int);

      // 销毁对象时释放本类持有的资源；收尾不能抛出业务异常，也不能继续接收新请求。
      ~WorkerCleanup() {
        stopping = true;
        std::signal(SIGTERM, term);
        std::signal(SIGINT, interrupt);
      }
    } cleanup{previous_term, previous_int};

    // 后台工作和采集缓冲排空之前保持处理器有效。
    IntegrationHttpServer server(argv[3], {}, argc == 4);
    std::cout << "trajectory-integration.v1 listening=127.0.0.1:" << server.port() << std::endl;
    while (!stopping)
      ::poll(nullptr, 0, 200);
    return 0;
  } catch (const Error &error) {
    std::cerr << "integration: " << error.what() << '\n';
  } catch (...) {
    std::cerr << "integration: configuration or storage unavailable\n";
  }
  return 2;
}
} // 命名空间结束：trajectory::evolution
