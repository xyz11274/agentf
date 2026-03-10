#ifndef AGENTFLOW_DATA_VARIANT_UTILS_H
#define AGENTFLOW_DATA_VARIANT_UTILS_H

#include <cctype>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "agent/common.h"

namespace agentflow {
namespace detail {

inline void skip_json_ws(const std::string& text, size_t& index) {
  while(index < text.size() && std::isspace(static_cast<unsigned char>(text[index]))) {
    ++index;
  }
}

inline bool parse_json_string_token(const std::string& text, size_t& index, std::string& output) {
  if(index >= text.size() || text[index] != '"') {
    return false;
  }
  ++index;
  output.clear();
  while(index < text.size()) {
    const char ch = text[index++];
    if(ch == '"') {
      return true;
    }
    if(ch == '\\') {
      if(index >= text.size()) {
        return false;
      }
      const char esc = text[index++];
      switch(esc) {
        case '"': output.push_back('"'); break;
        case '\\': output.push_back('\\'); break;
        case '/': output.push_back('/'); break;
        case 'b': output.push_back('\b'); break;
        case 'f': output.push_back('\f'); break;
        case 'n': output.push_back('\n'); break;
        case 'r': output.push_back('\r'); break;
        case 't': output.push_back('\t'); break;
        default: output.push_back(esc); break;
      }
      continue;
    }
    output.push_back(ch);
  }
  return false;
}

inline bool skip_json_string_token(const std::string& text, size_t& index) {
  std::string ignored;
  return parse_json_string_token(text, index, ignored);
}

inline bool skip_json_value(const std::string& text, size_t& index);

inline bool skip_json_object(const std::string& text, size_t& index) {
  if(index >= text.size() || text[index] != '{') {
    return false;
  }
  ++index;
  while(index < text.size()) {
    skip_json_ws(text, index);
    if(index >= text.size()) {
      return false;
    }
    if(text[index] == '}') {
      ++index;
      return true;
    }
    std::string ignored_key;
    if(!parse_json_string_token(text, index, ignored_key)) {
      return false;
    }
    skip_json_ws(text, index);
    if(index >= text.size() || text[index] != ':') {
      return false;
    }
    ++index;
    if(!skip_json_value(text, index)) {
      return false;
    }
    skip_json_ws(text, index);
    if(index < text.size() && text[index] == ',') {
      ++index;
      continue;
    }
    if(index < text.size() && text[index] == '}') {
      ++index;
      return true;
    }
    return false;
  }
  return false;
}

inline bool skip_json_array(const std::string& text, size_t& index) {
  if(index >= text.size() || text[index] != '[') {
    return false;
  }
  ++index;
  while(index < text.size()) {
    skip_json_ws(text, index);
    if(index >= text.size()) {
      return false;
    }
    if(text[index] == ']') {
      ++index;
      return true;
    }
    if(!skip_json_value(text, index)) {
      return false;
    }
    skip_json_ws(text, index);
    if(index < text.size() && text[index] == ',') {
      ++index;
      continue;
    }
    if(index < text.size() && text[index] == ']') {
      ++index;
      return true;
    }
    return false;
  }
  return false;
}

inline bool skip_json_value(const std::string& text, size_t& index) {
  skip_json_ws(text, index);
  if(index >= text.size()) {
    return false;
  }
  const char ch = text[index];
  if(ch == '"') {
    return skip_json_string_token(text, index);
  }
  if(ch == '{') {
    return skip_json_object(text, index);
  }
  if(ch == '[') {
    return skip_json_array(text, index);
  }
  if(ch == 't' && text.compare(index, 4, "true") == 0) {
    index += 4;
    return true;
  }
  if(ch == 'f' && text.compare(index, 5, "false") == 0) {
    index += 5;
    return true;
  }
  if(ch == 'n' && text.compare(index, 4, "null") == 0) {
    index += 4;
    return true;
  }
  if(ch == '-' || ch == '+' || std::isdigit(static_cast<unsigned char>(ch))) {
    ++index;
    while(index < text.size()) {
      const char current = text[index];
      if(std::isdigit(static_cast<unsigned char>(current)) || current == '.' || current == 'e' ||
         current == 'E' || current == '+' || current == '-') {
        ++index;
        continue;
      }
      break;
    }
    return true;
  }
  return false;
}

inline bool extract_json_value(const std::string& text, size_t& index, std::string& output) {
  skip_json_ws(text, index);
  const size_t start = index;
  if(!skip_json_value(text, index)) {
    return false;
  }
  output = text.substr(start, index - start);
  return true;
}

inline std::string trim_copy(const std::string& value) {
  size_t start = 0;
  while(start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
    ++start;
  }
  size_t end = value.size();
  while(end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(start, end - start);
}

inline bool parse_json_string_value(const std::string& raw, std::string& output) {
  std::string trimmed = trim_copy(raw);
  if(trimmed == "null") {
    output.clear();
    return true;
  }
  size_t index = 0;
  if(!parse_json_string_token(trimmed, index, output)) {
    return false;
  }
  skip_json_ws(trimmed, index);
  return index == trimmed.size();
}

inline bool parse_json_int64_value(const std::string& raw, int64_t& output) {
  std::string trimmed = trim_copy(raw);
  try {
    if(!trimmed.empty() && trimmed.front() == '"') {
      std::string string_value;
      if(!parse_json_string_value(trimmed, string_value)) {
        return false;
      }
      output = static_cast<int64_t>(std::stoll(string_value));
      return true;
    }
    output = static_cast<int64_t>(std::stoll(trimmed));
    return true;
  } catch(...) {
    return false;
  }
}

inline bool parse_json_double_value(const std::string& raw, double& output) {
  std::string trimmed = trim_copy(raw);
  try {
    if(!trimmed.empty() && trimmed.front() == '"') {
      std::string string_value;
      if(!parse_json_string_value(trimmed, string_value)) {
        return false;
      }
      output = std::stod(string_value);
      return true;
    }
    output = std::stod(trimmed);
    return true;
  } catch(...) {
    return false;
  }
}

inline bool looks_like_json_value(const std::string& raw) {
  std::string trimmed = trim_copy(raw);
  if(trimmed.empty()) {
    return false;
  }
  size_t index = 0;
  if(!skip_json_value(trimmed, index)) {
    return false;
  }
  skip_json_ws(trimmed, index);
  return index == trimmed.size();
}

}  // namespace detail

inline bool is_file_descriptor_value(const FileDescriptor& descriptor) {
  return !descriptor.file_id.empty() || !descriptor.uri.empty() ||
         !descriptor.storage_backend.empty() || !descriptor.media_type.empty();
}

inline std::string escape_json_string(const std::string& input) {
  std::ostringstream oss;
  for(const char ch : input) {
    switch(ch) {
      case '"': oss << "\\\""; break;
      case '\\': oss << "\\\\"; break;
      case '\b': oss << "\\b"; break;
      case '\f': oss << "\\f"; break;
      case '\n': oss << "\\n"; break;
      case '\r': oss << "\\r"; break;
      case '\t': oss << "\\t"; break;
      default: oss << ch; break;
    }
  }
  return oss.str();
}

inline std::string file_descriptor_to_json(const FileDescriptor& descriptor) {
  std::ostringstream oss;
  oss << '{'
      << "\"file_id\":\"" << escape_json_string(descriptor.file_id) << "\"," 
      << "\"uri\":\"" << escape_json_string(descriptor.uri) << "\"," 
      << "\"storage_backend\":\"" << escape_json_string(descriptor.storage_backend) << "\"," 
      << "\"media_type\":\"" << escape_json_string(descriptor.media_type) << "\"," 
      << "\"size_bytes\":" << descriptor.size_bytes << ','
      << "\"checksum\":\"" << escape_json_string(descriptor.checksum) << "\"," 
      << "\"created_at\":" << descriptor.created_at << ','
      << "\"ttl_seconds\":" << descriptor.ttl_seconds << ','
      << "\"owner\":\"" << escape_json_string(descriptor.owner) << "\"," 
      << "\"metadata\":";
  if(descriptor.metadata_json.empty()) {
    oss << "{}";
  } else if(detail::looks_like_json_value(descriptor.metadata_json)) {
    oss << descriptor.metadata_json;
  } else {
    oss << '"' << escape_json_string(descriptor.metadata_json) << '"';
  }
  oss << '}';
  return oss.str();
}

inline std::string file_descriptor_list_to_json(const FileDescriptorList& descriptors) {
  std::ostringstream oss;
  oss << '[';
  for(size_t i = 0; i < descriptors.size(); ++i) {
    if(i != 0) {
      oss << ',';
    }
    oss << file_descriptor_to_json(descriptors[i]);
  }
  oss << ']';
  return oss.str();
}

inline bool parse_file_descriptor_json(const std::string& json, FileDescriptor& descriptor) {
  FileDescriptor parsed;
  size_t index = 0;
  detail::skip_json_ws(json, index);
  if(index >= json.size() || json[index] != '{') {
    return false;
  }
  ++index;
  detail::skip_json_ws(json, index);
  if(index < json.size() && json[index] == '}') {
    return false;
  }

  while(index < json.size()) {
    detail::skip_json_ws(json, index);
    std::string key;
    if(!detail::parse_json_string_token(json, index, key)) {
      return false;
    }
    detail::skip_json_ws(json, index);
    if(index >= json.size() || json[index] != ':') {
      return false;
    }
    ++index;

    std::string raw_value;
    if(!detail::extract_json_value(json, index, raw_value)) {
      return false;
    }

    if(key == "file_id") {
      if(!detail::parse_json_string_value(raw_value, parsed.file_id)) return false;
    } else if(key == "uri") {
      if(!detail::parse_json_string_value(raw_value, parsed.uri)) return false;
    } else if(key == "storage_backend") {
      if(!detail::parse_json_string_value(raw_value, parsed.storage_backend)) return false;
    } else if(key == "media_type") {
      if(!detail::parse_json_string_value(raw_value, parsed.media_type)) return false;
    } else if(key == "size_bytes") {
      if(!detail::parse_json_int64_value(raw_value, parsed.size_bytes)) return false;
    } else if(key == "checksum") {
      if(!detail::parse_json_string_value(raw_value, parsed.checksum)) return false;
    } else if(key == "created_at") {
      if(!detail::parse_json_int64_value(raw_value, parsed.created_at)) return false;
    } else if(key == "ttl" || key == "ttl_seconds") {
      if(!detail::parse_json_int64_value(raw_value, parsed.ttl_seconds)) return false;
    } else if(key == "owner") {
      if(!detail::parse_json_string_value(raw_value, parsed.owner)) return false;
    } else if(key == "metadata") {
      const std::string trimmed = detail::trim_copy(raw_value);
      if(!trimmed.empty() && trimmed.front() == '"') {
        if(!detail::parse_json_string_value(trimmed, parsed.metadata_json)) return false;
      } else {
        parsed.metadata_json = trimmed;
      }
    }

    detail::skip_json_ws(json, index);
    if(index >= json.size()) {
      return false;
    }
    if(json[index] == '}') {
      ++index;
      detail::skip_json_ws(json, index);
      if(index != json.size()) {
        return false;
      }
      if(!is_file_descriptor_value(parsed)) {
        return false;
      }
      descriptor = std::move(parsed);
      return true;
    }
    if(json[index] != ',') {
      return false;
    }
    ++index;
  }
  return false;
}

inline bool parse_file_descriptor_list_json(const std::string& json, FileDescriptorList& descriptors) {
  FileDescriptorList parsed;
  size_t index = 0;
  detail::skip_json_ws(json, index);
  if(index >= json.size() || json[index] != '[') {
    return false;
  }
  ++index;
  detail::skip_json_ws(json, index);
  if(index < json.size() && json[index] == ']') {
    ++index;
    detail::skip_json_ws(json, index);
    if(index != json.size()) {
      return false;
    }
    descriptors = std::move(parsed);
    return true;
  }

  while(index < json.size()) {
    std::string raw_value;
    if(!detail::extract_json_value(json, index, raw_value)) {
      return false;
    }
    FileDescriptor descriptor;
    if(!parse_file_descriptor_json(raw_value, descriptor)) {
      return false;
    }
    parsed.push_back(std::move(descriptor));
    detail::skip_json_ws(json, index);
    if(index >= json.size()) {
      return false;
    }
    if(json[index] == ']') {
      ++index;
      detail::skip_json_ws(json, index);
      if(index != json.size()) {
        return false;
      }
      descriptors = std::move(parsed);
      return true;
    }
    if(json[index] != ',') {
      return false;
    }
    ++index;
  }
  return false;
}

inline std::string data_variant_to_string(const DataVariant& value) {
  return std::visit(
      [](const auto& arg) -> std::string {
        using T = std::decay_t<decltype(arg)>;
        if constexpr(std::is_same_v<T, std::string>) {
          return arg;
        } else if constexpr(std::is_same_v<T, bool>) {
          return arg ? "true" : "false";
        } else if constexpr(std::is_same_v<T, int64_t>) {
          return std::to_string(arg);
        } else if constexpr(std::is_same_v<T, double>) {
          std::ostringstream oss;
          oss << arg;
          return oss.str();
        } else if constexpr(std::is_same_v<T, FileDescriptor>) {
          return file_descriptor_to_json(arg);
        } else {
          return file_descriptor_list_to_json(arg);
        }
      },
      value);
}

inline std::string data_variant_type_name(const DataVariant& value) {
  return std::visit(
      [](const auto& arg) -> std::string {
        using T = std::decay_t<decltype(arg)>;
        if constexpr(std::is_same_v<T, std::string>) {
          return "string";
        } else if constexpr(std::is_same_v<T, bool>) {
          return "bool";
        } else if constexpr(std::is_same_v<T, int64_t>) {
          return "int";
        } else if constexpr(std::is_same_v<T, double>) {
          return "float";
        } else if constexpr(std::is_same_v<T, FileDescriptor>) {
          return "file";
        } else {
          return "file_list";
        }
      },
      value);
}

inline bool data_variant_from_encoded_value(const std::string& type, const std::string& payload,
                                            DataVariant& value) {
  try {
    if(type == "string") {
      value = payload;
      return true;
    }
    if(type == "bool") {
      value = (payload == "true" || payload == "1");
      return true;
    }
    if(type == "float") {
      value = std::stod(payload);
      return true;
    }
    if(type == "int") {
      value = static_cast<int64_t>(std::stoll(payload));
      return true;
    }
    if(type == "file") {
      FileDescriptor descriptor;
      if(!parse_file_descriptor_json(payload, descriptor)) {
        return false;
      }
      value = std::move(descriptor);
      return true;
    }
    if(type == "file_list") {
      FileDescriptorList descriptors;
      if(!parse_file_descriptor_list_json(payload, descriptors)) {
        return false;
      }
      value = std::move(descriptors);
      return true;
    }
  } catch(...) {
    return false;
  }
  return false;
}

inline bool parse_json_object_to_datamap(const std::string& json, DataMap& out) {
  size_t index = 0;
  while(index < json.size() && json[index] != '{') {
    while(index < json.size() && json[index] != '\n') {
      ++index;
    }
    if(index < json.size()) {
      ++index;
    }
    detail::skip_json_ws(json, index);
  }
  if(index >= json.size() || json[index] != '{') {
    return false;
  }
  ++index;
  detail::skip_json_ws(json, index);
  if(index < json.size() && json[index] == '}') {
    return true;
  }

  while(index < json.size()) {
    detail::skip_json_ws(json, index);
    std::string key;
    if(!detail::parse_json_string_token(json, index, key)) {
      return false;
    }
    detail::skip_json_ws(json, index);
    if(index >= json.size() || json[index] != ':') {
      return false;
    }
    ++index;

    std::string raw_value;
    if(!detail::extract_json_value(json, index, raw_value)) {
      return false;
    }

    const std::string trimmed = detail::trim_copy(raw_value);
    if(!trimmed.empty() && trimmed.front() == '"') {
      std::string parsed;
      if(!detail::parse_json_string_value(trimmed, parsed)) {
        return false;
      }
      out[key] = parsed;
    } else if(trimmed == "true") {
      out[key] = true;
    } else if(trimmed == "false") {
      out[key] = false;
    } else if(trimmed == "null") {
      out[key] = std::string("");
    } else if(!trimmed.empty() && trimmed.front() == '{') {
      FileDescriptor descriptor;
      if(parse_file_descriptor_json(trimmed, descriptor)) {
        out[key] = std::move(descriptor);
      } else {
        out[key] = trimmed;
      }
    } else if(!trimmed.empty() && trimmed.front() == '[') {
      FileDescriptorList descriptors;
      if(parse_file_descriptor_list_json(trimmed, descriptors)) {
        out[key] = std::move(descriptors);
      } else {
        out[key] = trimmed;
      }
    } else {
      bool is_float = trimmed.find('.') != std::string::npos || trimmed.find('e') != std::string::npos ||
                      trimmed.find('E') != std::string::npos;
      try {
        if(is_float) {
          out[key] = std::stod(trimmed);
        } else {
          out[key] = static_cast<int64_t>(std::stoll(trimmed));
        }
      } catch(...) {
        out[key] = trimmed;
      }
    }

    detail::skip_json_ws(json, index);
    if(index >= json.size()) {
      return false;
    }
    if(json[index] == '}') {
      return true;
    }
    if(json[index] != ',') {
      return false;
    }
    ++index;
  }
  return false;
}

}  // namespace agentflow

#endif  // AGENTFLOW_DATA_VARIANT_UTILS_H
