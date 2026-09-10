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

// Construct once before serving business requests. invoke performs bounded local
// work only: no socket, filesystem or evaluation. Per-run contention has a small
// configurable budget; background I/O never holds a business run lock.
// Busy/capacity/failure is explicit degradation, not a successful observation.
class LocalRuntime {
 public:
  LocalRuntime(const std::filesystem::path& config, const std::string& bearer,
               LocalRuntimeOptions options = {});
  ~LocalRuntime();
  LocalRuntime(const LocalRuntime&) = delete;
  LocalRuntime& operator=(const LocalRuntime&) = delete;
  IntegrationResponse invoke(const std::string& request) noexcept;
  std::string metrics() const;
  // Pause only evolution additions; never alter the host's own safeguards.
  // Resuming applies to new runs. Interrupted runs remain quarantined.
  void set_enabled(bool enabled) noexcept;
  // These may block: maintenance/test/shutdown use only, not a business boundary.
  IntegrationResponse manage(const std::string& request) noexcept;
  void drain();
  void evaluate_pending();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace trajectory::evolution
