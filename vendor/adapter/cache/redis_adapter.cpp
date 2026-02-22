#include "vendor/cache/redis_adapter.h"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace agentflow {
namespace vendor {
namespace adapter {

bool RedisAdapter::connect(const std::string& address) {
  address_ = address;
  connected_ = true;
  return true;
}

void RedisAdapter::disconnect() {
  connected_ = false;
}

bool RedisAdapter::is_connected() const {
  return connected_;
}

bool RedisAdapter::set(const std::string& key, const std::string& value) {
  kv_cache_[key] = value;
  if(!connected_ || !has_redis_cli()) {
    return true;
  }
  const std::string cmd = "-u " + shell_escape(address_) + " SET " + shell_escape(key) + " "
                          + shell_escape(value);
  return run_redis_cli(cmd);
}

bool RedisAdapter::get(const std::string& key, std::string& value) const {
  auto it = kv_cache_.find(key);
  if(it != kv_cache_.end()) {
    value = it->second;
    return true;
  }
  return false;
}

bool RedisAdapter::del(const std::string& key) {
  kv_cache_.erase(key);
  if(!connected_ || !has_redis_cli()) {
    return true;
  }
  const std::string cmd = "-u " + shell_escape(address_) + " DEL " + shell_escape(key);
  return run_redis_cli(cmd);
}

bool RedisAdapter::sadd(const std::string& index_key, const std::string& value) {
  set_cache_[index_key].insert(value);
  if(!connected_ || !has_redis_cli()) {
    return true;
  }
  const std::string cmd = "-u " + shell_escape(address_) + " SADD " + shell_escape(index_key)
                          + " " + shell_escape(value);
  return run_redis_cli(cmd);
}

std::vector<std::string> RedisAdapter::smembers(const std::string& index_key) const {
  std::vector<std::string> out;
  auto it = set_cache_.find(index_key);
  if(it == set_cache_.end()) {
    return out;
  }
  for(const auto& v : it->second) {
    out.push_back(v);
  }
  return out;
}

bool RedisAdapter::run_redis_cli(const std::string& command) const {
  const std::string full = "redis-cli " + command + " >/dev/null 2>&1";
  return std::system(full.c_str()) == 0;
}

bool RedisAdapter::has_redis_cli() const {
  return std::system("command -v redis-cli >/dev/null 2>&1") == 0;
}

std::string RedisAdapter::shell_escape(const std::string& raw) const {
  std::string escaped = "'";
  for(char c : raw) {
    if(c == '\'') {
      escaped += "'\\''";
    } else {
      escaped.push_back(c);
    }
  }
  escaped += "'";
  return escaped;
}

}  // namespace adapter
}  // namespace vendor
}  // namespace agentflow
