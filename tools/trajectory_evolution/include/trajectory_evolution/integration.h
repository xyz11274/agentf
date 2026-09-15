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

// Language-neutral event/control API. The same implementation serves HTTP and
// in-process users; host execution is never delegated to the component.
class IntegrationService {
 public:
  explicit IntegrationService(const std::filesystem::path& config,
      const std::map<std::string, std::string>& environment = {});
  ~IntegrationService();
  IntegrationService(const IntegrationService&) = delete;
  IntegrationService& operator=(const IntegrationService&) = delete;
  IntegrationResponse call(const std::string& bearer, const std::string& request);
  // Trusted local import, not an HTTP path-reading endpoint. Completed historical
  // logs enter the same durable queue as live runs; exact reimports are no-ops.
  IntegrationResponse import_file(const std::filesystem::path& path);
  // Explicit cold-start admission: only verified, unmodified L0 training trials.
  IntegrationResponse import_training_evaluation(const std::filesystem::path& report_directory,
      const std::filesystem::path& dataset_directory);
  std::string learning_status() const;
  void process_evaluations();
  // Explicit maintenance may flush a small batch; background workers obey cadence.
  void process_evaluations(bool flush);
  std::uint64_t work_version() const;
  void wait_for_work(std::uint64_t observed, unsigned max_wait_ms);
  void wake_background();
  unsigned port() const;
  unsigned http_timeout_ms() const;
  unsigned workers() const;
  std::size_t max_request_bytes() const;
  // Maintenance only. Wait for already accepted buffered capture, not learning.
  bool flush_capture(unsigned timeout_ms = 5000);
  bool capture_finished() const;
  bool capture_finished(std::size_t expected_runs) const;
  std::string evaluation_entry_status() const;

 private:
  friend class LocalRuntime;
  IntegrationResponse import_source(const std::filesystem::path& path,
      const std::string& training_trace, const std::string& training_source);
  IntegrationResponse import_training_report(const std::filesystem::path& report_directory,
      const std::filesystem::path& dataset_directory, const std::string& baseline_json);
  void refresh_agent_evaluations();
  // Startup/background bridge only; never exposed through the HTTP dispatcher.
  std::string local_environment(const std::string& bearer, std::uint64_t memory_version = 0);
  IntegrationResponse persist_local(const std::string& bearer, const std::string& state);
  void checkpoint_local(const std::string& bearer, const std::string& instance, const std::string& metrics);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Owns a loopback listener and background workers; usable by isolated evaluators.
class IntegrationHttpServer {
 public:
  explicit IntegrationHttpServer(const std::filesystem::path& config,
      const std::map<std::string, std::string>& environment = {});
  ~IntegrationHttpServer();
  unsigned port() const;
  bool flush_capture(unsigned timeout_ms = 5000);
  bool capture_finished() const;
  bool capture_finished(std::size_t expected_runs) const;
  std::string evaluation_entry_status() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

int run_integration_server(int argc, char** argv);

}  // namespace trajectory::evolution
