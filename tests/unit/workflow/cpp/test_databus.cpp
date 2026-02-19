#include <cassert>

#include "runtime/communication/databus.h"

using namespace agentflow;
using namespace agentflow::runtime;

void test_databus() {
  // Create a high-speed databus
  HighSpeedDataBus databus(1024 * 1024);  // 1MB capacity

  // Test put and get
  assert(databus.put("key1", std::string("value1")));

  agentflow::DataVariant value;
  assert(databus.get("key1", value));
  assert(std::get<std::string>(value) == "value1");

  // Test contains
  assert(databus.contains("key1"));
  assert(!databus.contains("nonexistent"));

  // Test capacity
  assert(databus.get_used_capacity() > 0);
  assert(databus.get_total_capacity() == 1024 * 1024);
  assert(databus.get_data_count() == 1);

  // Test remove
  assert(databus.remove("key1"));
  assert(!databus.contains("key1"));
  assert(databus.get_data_count() == 0);

  // Additional test: verify multiple entries
  assert(databus.put("key3", "value3"));
  assert(databus.put("key4", "value4"));
  assert(databus.get_data_count() == 2);
}
