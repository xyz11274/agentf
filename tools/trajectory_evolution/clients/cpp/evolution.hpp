#pragma once
#include "../c/evolution.h"
#include <memory>
#include <stdexcept>
#include <string>

namespace trajectory_evolution_http {
struct Error : std::runtime_error {
  long status;
  Error(const char* code, long value) : std::runtime_error(code), status(value) {}
};
// No dependency on a model SDK or a particular JSON library. Initialize libcurl
// once with te_initialize() before threads; deserialize the returned envelope.
class Client {
 public:
  Client(std::string url, std::string token, long timeout_ms = 7000,
         std::size_t max_response_bytes = 2097152, unsigned retries = 1)
      : url_(std::move(url)), token_(std::move(token)), timeout_(timeout_ms), limit_(max_response_bytes), retries_(retries) {}
  std::string invoke(const std::string& request_json) const {
    te_client client{url_.c_str(), token_.c_str(), timeout_, limit_, retries_};
    char* result = nullptr; const char* code = nullptr; long status = 0;
    if(te_invoke(&client, request_json.c_str(), &result, &status, &code) != 0) throw Error(code, status);
    std::unique_ptr<char, decltype(&te_free)> buffer(result, &te_free);
    return std::string(buffer.get());
  }
 private:
  std::string url_, token_;
  long timeout_; std::size_t limit_; unsigned retries_;
};
}  // namespace trajectory_evolution_http
