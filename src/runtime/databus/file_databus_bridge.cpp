#include "runtime/databus/file_databus_bridge.h"

#include "agent/data_variant_utils.h"

#include <rapidjson/document.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace agentflow {
namespace runtime {
namespace {

std::string trim_copy(const std::string& input) {
  size_t begin = 0;
  while(begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin]))) {
    ++begin;
  }
  size_t end = input.size();
  while(end > begin && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
    --end;
  }
  return input.substr(begin, end - begin);
}

std::string json_unescape(const std::string& input) {
  std::string output;
  output.reserve(input.size());
  for(size_t i = 0; i < input.size(); ++i) {
    char ch = input[i];
    if(ch == '\\' && i + 1 < input.size()) {
      char next = input[++i];
      switch(next) {
        case '"':
          output.push_back('"');
          break;
        case '\\':
          output.push_back('\\');
          break;
        case 'n':
          output.push_back('\n');
          break;
        case 'r':
          output.push_back('\r');
          break;
        case 't':
          output.push_back('\t');
          break;
        default:
          output.push_back(next);
          break;
      }
    } else {
      output.push_back(ch);
    }
  }
  return output;
}

}  // namespace

FileDataBusBridge::FileDataBusBridge(const std::string& root_dir)
    : root_dir_(root_dir.empty() ? make_temp_root() : root_dir), owns_root_(root_dir.empty()) {
  kv_dir_     = root_dir_ / "kv";
  events_dir_ = root_dir_ / "events";
}

bool FileDataBusBridge::initialize() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::error_code             ec;
  std::filesystem::create_directories(kv_dir_, ec);
  if(ec) {
    return false;
  }
  std::filesystem::create_directories(events_dir_, ec);
  return !ec;
}

void FileDataBusBridge::cleanup() {
  if(!owns_root_) {
    return;
  }
  std::error_code ec;
  std::filesystem::remove_all(root_dir_, ec);
}

bool FileDataBusBridge::put(const std::string& key, const DataVariant& value) {
  if(key.empty()) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream               ofs(key_path(key), std::ios::trunc);
    if(!ofs.is_open()) {
      return false;
    }
    EncodedValue encoded = encode_value(value);
    ofs << "key=" << key << "\n";
    ofs << "type=" << encoded.type << "\n";
    ofs << "value_b64=" << encoded.encoded_value << "\n";
    ofs << "timestamp_ms=" << now_ms() << "\n";
  }
  return publish(key, value, "set");
}

bool FileDataBusBridge::get(const std::string& key, DataVariant& value) const {
  std::string type;
  std::string encoded_value;
  if(!read_key_file(key_path(key), type, encoded_value)) {
    return false;
  }
  return decode_value(type, encoded_value, value);
}

bool FileDataBusBridge::publish(const std::string& topic, const DataVariant& value,
                                const std::string& kind) {
  if(topic.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  EncodedValue                encoded = encode_value(value);
  static std::atomic<uint64_t> counter{0};
  const uint64_t seq = counter.fetch_add(1);
  const auto event_file = events_dir_ / (std::to_string(now_ms()) + "_" + std::to_string(seq) + ".evt");

  std::ofstream ofs(event_file, std::ios::trunc);
  if(!ofs.is_open()) {
    return false;
  }
  ofs << "topic=" << topic << "\n";
  ofs << "kind=" << kind << "\n";
  ofs << "type=" << encoded.type << "\n";
  ofs << "value_b64=" << encoded.encoded_value << "\n";
  ofs << "timestamp_ms=" << now_ms() << "\n";
  return true;
}

std::vector<FileDataBusBridge::EventRecord> FileDataBusBridge::poll_events(
    std::set<std::string>& seen_files) const {
  std::vector<EventRecord> records;
  std::error_code          ec;
  if(!std::filesystem::exists(events_dir_, ec)) {
    return records;
  }

  std::vector<std::filesystem::directory_entry> entries;
  for(const auto& entry : std::filesystem::directory_iterator(events_dir_, ec)) {
    if(ec || !entry.is_regular_file()) {
      continue;
    }
    entries.push_back(entry);
  }

  std::sort(entries.begin(), entries.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.path().filename() < rhs.path().filename(); });

  for(const auto& entry : entries) {
    const std::string file_name = entry.path().filename().string();
    if(seen_files.count(file_name) > 0) {
      continue;
    }
    EventRecord record;
    if(read_event_file(entry.path(), record)) {
      record.file_name = file_name;
      records.push_back(record);
      seen_files.insert(file_name);
    }
  }
  return records;
}

std::map<std::string, DataVariant> FileDataBusBridge::snapshot() const {
  std::map<std::string, DataVariant> data;
  std::error_code                    ec;
  if(!std::filesystem::exists(kv_dir_, ec)) {
    return data;
  }

  for(const auto& entry : std::filesystem::directory_iterator(kv_dir_, ec)) {
    if(ec || !entry.is_regular_file()) {
      continue;
    }
    std::string type;
    std::string encoded_value;
    if(!read_key_file(entry.path(), type, encoded_value)) {
      continue;
    }
    DataVariant value;
    if(!decode_value(type, encoded_value, value)) {
      continue;
    }
    data[decode_key(entry.path().stem().string())] = value;
  }
  return data;
}

std::string FileDataBusBridge::make_temp_root() {
  auto base = std::filesystem::temp_directory_path() / "agentflow_databus";
  std::error_code ec;
  std::filesystem::create_directories(base, ec);
  std::mt19937_64 rng(
      static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
  return (base / (std::to_string(now_ms()) + "_" + std::to_string(rng()))).string();
}

bool FileDataBusBridge::parse_scalar_json_object(const std::string&               json,
                                                 std::map<std::string, std::string>& out) {
  rapidjson::Document d;
  d.Parse(json.c_str());
  if (d.HasParseError() || !d.IsObject()) return false;

  for (auto it = d.MemberBegin(); it != d.MemberEnd(); ++it) {
    const auto& val = it->value;
    if (val.IsString())         out[it->name.GetString()] = val.GetString();
    else if (val.IsInt64())     out[it->name.GetString()] = std::to_string(val.GetInt64());
    else if (val.IsInt())       out[it->name.GetString()] = std::to_string(val.GetInt());
    else if (val.IsDouble())    out[it->name.GetString()] = std::to_string(val.GetDouble());
    else if (val.IsBool())      out[it->name.GetString()] = val.GetBool() ? "true" : "false";
    else if (val.IsNull())      out[it->name.GetString()] = "null";
    else                        return false; // nested objects/arrays not allowed
  }
  return true;
}

std::string FileDataBusBridge::encode_key(const std::string& key) {
  std::ostringstream oss;
  for(unsigned char ch : key) {
    if(std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
      oss << static_cast<char>(ch);
    } else {
      oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
          << static_cast<int>(ch) << std::nouppercase << std::dec;
    }
  }
  return oss.str();
}

std::string FileDataBusBridge::decode_key(const std::string& encoded) {
  std::string output;
  for(size_t i = 0; i < encoded.size(); ++i) {
    if(encoded[i] == '%' && i + 2 < encoded.size()) {
      const std::string hex = encoded.substr(i + 1, 2);
      char*             end = nullptr;
      const long        value = std::strtol(hex.c_str(), &end, 16);
      if(end != nullptr && *end == '\0') {
        output.push_back(static_cast<char>(value));
        i += 2;
        continue;
      }
    }
    output.push_back(encoded[i]);
  }
  return output;
}

FileDataBusBridge::EncodedValue FileDataBusBridge::encode_value(const DataVariant& value) {
  return {data_variant_type_name(value), base64_encode(data_variant_to_string(value))};
}

bool FileDataBusBridge::decode_value(const std::string& type, const std::string& encoded_value,
                                     DataVariant& value) {
  return data_variant_from_encoded_value(type, base64_decode(encoded_value), value);
}

std::string FileDataBusBridge::base64_encode(const std::string& input) {
  static const char* alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve(((input.size() + 2) / 3) * 4);
  int val = 0;
  int valb = -6;
  for(unsigned char ch : input) {
    val = (val << 8) + ch;
    valb += 8;
    while(valb >= 0) {
      output.push_back(alphabet[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if(valb > -6) {
    output.push_back(alphabet[((val << 8) >> (valb + 8)) & 0x3F]);
  }
  while(output.size() % 4 != 0) {
    output.push_back('=');
  }
  return output;
}

std::string FileDataBusBridge::base64_decode(const std::string& input) {
  static std::vector<int> table(256, -1);
  static const bool       initialized = []() {
    const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for(size_t i = 0; i < alphabet.size(); ++i) {
      table[static_cast<unsigned char>(alphabet[i])] = static_cast<int>(i);
    }
    return true;
  }();
  (void)initialized;

  std::string output;
  int         val = 0;
  int         valb = -8;
  for(unsigned char ch : input) {
    if(std::isspace(ch) || ch == '=') {
      continue;
    }
    if(table[ch] == -1) {
      break;
    }
    val = (val << 6) + table[ch];
    valb += 6;
    if(valb >= 0) {
      output.push_back(static_cast<char>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return output;
}

int64_t FileDataBusBridge::now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string FileDataBusBridge::read_metadata_line(const std::filesystem::path& file,
                                                  const std::string&           key) {
  std::ifstream ifs(file);
  std::string   line;
  const std::string prefix = key + "=";
  while(std::getline(ifs, line)) {
    if(line.rfind(prefix, 0) == 0) {
      return line.substr(prefix.size());
    }
  }
  return "";
}

bool FileDataBusBridge::read_key_file(const std::filesystem::path& file, std::string& type,
                                      std::string& encoded_value) {
  if(!std::filesystem::exists(file)) {
    return false;
  }
  type          = read_metadata_line(file, "type");
  encoded_value = read_metadata_line(file, "value_b64");
  return !type.empty();
}

bool FileDataBusBridge::read_event_file(const std::filesystem::path& file, EventRecord& record) {
  std::ifstream ifs(file);
  if(!ifs.is_open()) {
    return false;
  }
  std::string type;
  std::string encoded_value;
  std::string line;
  while(std::getline(ifs, line)) {
    const size_t pos = line.find('=');
    if(pos == std::string::npos) {
      continue;
    }
    const std::string key   = line.substr(0, pos);
    const std::string value = line.substr(pos + 1);
    if(key == "topic") {
      record.topic = value;
    } else if(key == "kind") {
      record.kind = value;
    } else if(key == "type") {
      type = value;
    } else if(key == "value_b64") {
      encoded_value = value;
    } else if(key == "timestamp_ms") {
      record.timestamp_ms = std::stoll(value);
    }
  }
  if(record.topic.empty() || type.empty()) {
    return false;
  }
  return decode_value(type, encoded_value, record.value);
}

std::filesystem::path FileDataBusBridge::key_path(const std::string& key) const {
  return kv_dir_ / (encode_key(key) + ".kv");
}

}  // namespace runtime
}  // namespace agentflow
