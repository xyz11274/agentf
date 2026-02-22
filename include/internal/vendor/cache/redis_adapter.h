#ifndef AGENTFLOW_VENDOR_CACHE_REDIS_ADAPTER_H
#define AGENTFLOW_VENDOR_CACHE_REDIS_ADAPTER_H

#include <map>
#include <set>
#include <string>
#include <vector>

namespace agentflow {
namespace vendor {
namespace adapter {

class RedisAdapter {
public:
  RedisAdapter() = default;
  ~RedisAdapter() = default;

  bool connect(const std::string& address);
  void disconnect();
  bool is_connected() const;

  bool set(const std::string& key, const std::string& value);
  bool get(const std::string& key, std::string& value) const;
  bool del(const std::string& key);

  bool sadd(const std::string& index_key, const std::string& value);
  std::vector<std::string> smembers(const std::string& index_key) const;

private:
  bool run_redis_cli(const std::string& command) const;
  bool has_redis_cli() const;
  std::string shell_escape(const std::string& raw) const;

  bool connected_ = false;
  std::string address_;
  std::map<std::string, std::string> kv_cache_;
  std::map<std::string, std::set<std::string>> set_cache_;
};

}  // namespace adapter
}  // namespace vendor
}  // namespace agentflow

#endif  // AGENTFLOW_VENDOR_CACHE_REDIS_ADAPTER_H
