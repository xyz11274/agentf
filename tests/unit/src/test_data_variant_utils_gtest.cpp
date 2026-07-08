// Regression tests for parse_json_object_to_datamap (data_variant_utils.h).
//
// Invariant I3: when the parser hits an incomplete JSON object (e.g. a node
// killed mid-stream at timeout), it MUST emit an INFO record (not WARN). The
// parser is never the root cause of a timeout kill — that failure is already
// reported by the caller via [NODE_EXECUTE_FAILED] — so a WARN here misleads
// operators.

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "agent/common.h"
#include "agent/data_variant_utils.h"
#include "utils/log_sink.h"
#include "utils/logger.h"

namespace {

using agentflow::DataMap;
using agentflow::ILogSink;
using agentflow::Logger;
using agentflow::parse_json_object_to_datamap;

// Records every log record with its numeric level so tests can assert on BOTH
// message content AND the level. Logger::Level: DEBUG=0, INFO=1, WARN=2, ERROR=3.
class CapturingLogSink : public ILogSink {
 public:
  void write(int level, const std::string& timestamp, const std::string& message) override {
    (void)timestamp;
    std::lock_guard<std::mutex> lock(mutex_);
    records_.emplace_back(level, message);
  }

  void flush() override {}

  std::string combined() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out;
    for (const auto& r : records_) {
      out += r.second;
      out += '\n';
    }
    return out;
  }

  int level_for(const std::string& needle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& r : records_) {
      if (r.second.find(needle) != std::string::npos) {
        return r.first;
      }
    }
    return -1;
  }

 private:
  mutable std::mutex                       mutex_;
  std::vector<std::pair<int, std::string>> records_;
};

// Installs a capturing sink per test and restores default logging in TearDown
// so the global Logger singleton doesn't leak state across suites.
class DataVariantUtilsLogTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto sink = std::make_unique<CapturingLogSink>();
    sink_raw_ = sink.get();
    Logger::instance().set_sink(std::move(sink));
  }

  void TearDown() override { Logger::instance().set_sink(nullptr); }

  CapturingLogSink* sink_raw_ = nullptr;
};

// I3: parse_json_object_to_datamap on an incomplete JSON object MUST emit an
// INFO record (not WARN) at data_variant_utils.h:696. Pinning the level at
// runtime catches a warn regression that code review alone could miss.
TEST_F(DataVariantUtilsLogTest, ParseJsonObjectToDataMap_IncompleteEmitsInfoNotWarn) {
  const std::string partial = "{\"a\":\"1\",\"b\":\"2\",\"unterminated\":";

  DataMap out;
  const bool ok = parse_json_object_to_datamap(partial, out);

  // parsed_count counts completed objects (closing '}'), not keys: with no
  // closing brace the function returns false even though the complete prefix
  // ("a","b") was stored as a side effect.
  EXPECT_FALSE(ok);
  EXPECT_EQ(out.count("a"), 1u);
  EXPECT_EQ(out.count("b"), 1u);

  ASSERT_NE(sink_raw_, nullptr);
  const std::string logs = sink_raw_->combined();
  EXPECT_NE(logs.find("incomplete JSON object"), std::string::npos)
      << "expected an 'incomplete JSON object' record; captured logs:\n"
      << logs;

  // Core I3 assertion: the incomplete-object record MUST be INFO (1), not
  // WARN (2). Level 2 here means the warn regression returned.
  const int level = sink_raw_->level_for("incomplete JSON object");
  EXPECT_EQ(level, 1)
      << "incomplete-object record must be INFO(1); got level=" << level;
}

}  // namespace
