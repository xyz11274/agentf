// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License

#include <chrono>
#include <iostream>
#include <thread>

#include "runtime/databus/local_databus.h"
#include "utils/logger.h"

using namespace agentflow;

namespace {
std::string as_string(const DataVariant& value) {
  if(const auto* s = std::get_if<std::string>(&value)) {
    return *s;
  }
  if(const auto* i = std::get_if<int64_t>(&value)) {
    return std::to_string(*i);
  }
  if(const auto* d = std::get_if<double>(&value)) {
    return std::to_string(*d);
  }
  if(const auto* b = std::get_if<bool>(&value)) {
    return *b ? "true" : "false";
  }
  return "";
}
}  // namespace

int main() {
  Logger::instance().info("AgentFlow Databus Example");
  Logger::instance().info("========================");

  runtime::ZeroCopyDataBus databus(1024 * 1024);
  Logger::instance().info_f("capacity=%zu bytes", databus.get_total_capacity());

  databus.put("request.id", std::string("req-001"));
  databus.put("request.retry", 1);

  DataVariant value;
  if(databus.get("request.id", value)) {
    Logger::instance().info_f("request.id=%s", as_string(value).c_str());
  }

  databus.subscribe("request.retry", [](const std::string& key, const DataVariant& updated) {
    Logger::instance().info_f("subscription: %s=%s", key.c_str(), as_string(updated).c_str());
  });

  databus.put("request.retry", 2);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto* ptr = databus.get_data_ptr("request.retry");
  if(ptr != nullptr) {
    Logger::instance().info_f("zero-copy pointer value=%s", as_string(*ptr).c_str());
  }

  Logger::instance().info_f("used=%zu bytes count=%d", databus.get_used_capacity(), databus.get_data_count());
  return 0;
}
