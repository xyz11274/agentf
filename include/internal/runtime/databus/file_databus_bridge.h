#ifndef AGENTFLOW_RUNTIME_FILE_DATABUS_BRIDGE_H
#define AGENTFLOW_RUNTIME_FILE_DATABUS_BRIDGE_H

#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "agent/common.h"

namespace agentflow {
namespace runtime {

class FileDataBusBridge {
public:
  struct EventRecord {
    std::string file_name;
    std::string kind;
    std::string topic;
    DataVariant value;
    int64_t     timestamp_ms = 0;
  };

  explicit FileDataBusBridge(const std::string& root_dir = "");

  bool initialize();
  void cleanup();

  const std::filesystem::path& root_dir() const { return root_dir_; }
  const std::filesystem::path& kv_dir() const { return kv_dir_; }
  const std::filesystem::path& events_dir() const { return events_dir_; }

  bool put(const std::string& key, const DataVariant& value);
  bool get(const std::string& key, DataVariant& value) const;
  bool publish(const std::string& topic, const DataVariant& value,
               const std::string& kind = "pub");

  std::vector<EventRecord> poll_events(std::set<std::string>& seen_files) const;
  std::map<std::string, DataVariant> snapshot() const;

  static std::string make_temp_root();
  static bool        parse_scalar_json_object(const std::string&               json,
                                              std::map<std::string, std::string>& out);

private:
  struct EncodedValue {
    std::string type;
    std::string encoded_value;
  };

  std::filesystem::path root_dir_;
  std::filesystem::path kv_dir_;
  std::filesystem::path events_dir_;
  bool                  owns_root_ = false;
  mutable std::mutex    mutex_;

  static std::string    encode_key(const std::string& key);
  static std::string    decode_key(const std::string& encoded);
  static EncodedValue   encode_value(const DataVariant& value);
  static bool           decode_value(const std::string& type, const std::string& encoded_value,
                                     DataVariant& value);
  static std::string    base64_encode(const std::string& input);
  static std::string    base64_decode(const std::string& input);
  static int64_t        now_ms();
  static std::string    read_metadata_line(const std::filesystem::path& file,
                                           const std::string&           key);
  static bool           read_key_file(const std::filesystem::path& file, std::string& type,
                                      std::string& encoded_value);
  static bool           read_event_file(const std::filesystem::path& file, EventRecord& record);
  std::filesystem::path key_path(const std::string& key) const;
};

}  // namespace runtime
}  // namespace agentflow

#endif  // AGENTFLOW_RUNTIME_FILE_DATABUS_BRIDGE_H
