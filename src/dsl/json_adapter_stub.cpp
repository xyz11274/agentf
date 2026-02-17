// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Stub JSON adapter implementation for testing without rapidjson

#ifndef AGENTFLOW_WITH_RAPIDJSON

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "agentflow/dsl/json_adapter/stub_impl.h"

namespace agentflow {
namespace dsl {

// StubJsonValue implementation

StubJsonValue::StubJsonValue(const std::string& string_value)
    : type_(JsonValueType::JSON_STRING), string_value_(string_value) {
}

StubJsonValue::StubJsonValue(double number_value)
    : type_(JsonValueType::JSON_NUMBER), number_value_(number_value) {
}

StubJsonValue::StubJsonValue(bool bool_value)
    : type_(JsonValueType::JSON_BOOL), bool_value_(bool_value) {
}

StubJsonValue::~StubJsonValue() = default;

JsonValueType StubJsonValue::get_type() const {
  return type_;
}

bool StubJsonValue::is_null() const {
  return type_ == JsonValueType::JSON_NULL;
}

bool StubJsonValue::is_bool() const {
  return type_ == JsonValueType::JSON_BOOL;
}

bool StubJsonValue::is_number() const {
  return type_ == JsonValueType::JSON_NUMBER;
}

bool StubJsonValue::is_string() const {
  return type_ == JsonValueType::JSON_STRING;
}

bool StubJsonValue::is_array() const {
  return type_ == JsonValueType::JSON_ARRAY;
}

bool StubJsonValue::is_object() const {
  return type_ == JsonValueType::JSON_OBJECT;
}

bool StubJsonValue::as_bool() const {
  return bool_value_;
}

double StubJsonValue::as_double() const {
  return number_value_;
}

int64_t StubJsonValue::as_int64() const {
  return static_cast<int64_t>(number_value_);
}

uint64_t StubJsonValue::as_uint64() const {
  return static_cast<uint64_t>(number_value_);
}

std::string StubJsonValue::as_string() const {
  return string_value_;
}

size_t StubJsonValue::array_size() const {
  return array_value_.size();
}

std::shared_ptr<IJsonValue> StubJsonValue::array_at(size_t index) {
  if(index >= array_value_.size()) {
    return nullptr;
  }
  return array_value_[index];
}

std::shared_ptr<const IJsonValue> StubJsonValue::array_at(size_t index) const {
  if(index >= array_value_.size()) {
    return nullptr;
  }
  return array_value_[index];
}

bool StubJsonValue::has_member(const std::string& key) const {
  return object_value_.find(key) != object_value_.end();
}

std::shared_ptr<IJsonValue> StubJsonValue::get_member(const std::string& key) {
  auto it = object_value_.find(key);
  if(it == object_value_.end()) {
    return nullptr;
  }
  return it->second;
}

std::shared_ptr<const IJsonValue> StubJsonValue::get_member(const std::string& key) const {
  auto it = object_value_.find(key);
  if(it == object_value_.end()) {
    return nullptr;
  }
  return it->second;
}

std::vector<std::string> StubJsonValue::object_keys() const {
  std::vector<std::string> keys;
  for(const auto& pair : object_value_) {
    keys.push_back(pair.first);
  }
  return keys;
}

size_t StubJsonValue::object_size() const {
  return object_value_.size();
}

std::string StubJsonValue::to_string() const {
  std::stringstream ss;
  switch(type_) {
    case JsonValueType::JSON_NULL:
      ss << "null";
      break;
    case JsonValueType::JSON_BOOL:
      ss << (bool_value_ ? "true" : "false");
      break;
    case JsonValueType::JSON_NUMBER:
      ss << number_value_;
      break;
    case JsonValueType::JSON_STRING:
      ss << "\"" << string_value_ << "\"";
      break;
    case JsonValueType::JSON_ARRAY:
      ss << "[array]";
      break;
    case JsonValueType::JSON_OBJECT:
      ss << "{object}";
      break;
  }
  return ss.str();
}

void StubJsonValue::add_array_element(std::shared_ptr<IJsonValue> value) {
  type_ = JsonValueType::JSON_ARRAY;
  array_value_.push_back(value);
}

void StubJsonValue::set_object_member(const std::string& key, std::shared_ptr<IJsonValue> value) {
  type_              = JsonValueType::JSON_OBJECT;
  object_value_[key] = value;
}

// StubJsonDocument implementation

StubJsonDocument::StubJsonDocument() = default;

StubJsonDocument::~StubJsonDocument() = default;

bool StubJsonDocument::parse(const std::string& json_str) {
  valid_     = true;
  error_msg_ = "";
  root_      = std::make_shared<StubJsonValue>();

  size_t pos = 0;
  skip_whitespace(json_str, pos);

  if(pos >= json_str.length()) {
    error_msg_ = "Empty JSON string";
    valid_     = false;
    return false;
  }

  if(!parse_value(json_str, pos, root_)) {
    valid_ = false;
    return false;
  }

  return true;
}

bool StubJsonDocument::parse_file(const std::string& file_path) {
  // Read file content
  std::ifstream file(file_path);
  if(!file.is_open()) {
    error_msg_ = "Failed to open file: " + file_path;
    valid_     = false;
    return false;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  file.close();

  // Parse the file content
  return parse(buffer.str());
}

std::shared_ptr<IJsonValue> StubJsonDocument::root() {
  if(!root_) {
    root_ = std::make_shared<StubJsonValue>();
  }
  return root_;
}

std::shared_ptr<const IJsonValue> StubJsonDocument::root() const {
  if(!root_) {
    return std::make_shared<StubJsonValue>();
  }
  return root_;
}

std::string StubJsonDocument::get_error() const {
  return error_msg_;
}

bool StubJsonDocument::is_valid() const {
  return valid_;
}

std::string StubJsonDocument::to_string() const {
  if(root_) {
    return root_->to_string();
  }
  return "{}";
}

std::string StubJsonDocument::to_compact_string() const {
  return to_string();
}

void StubJsonDocument::skip_whitespace(const std::string& str, size_t& pos) {
  while(pos < str.length()
        && (str[pos] == ' ' || str[pos] == '\t' || str[pos] == '\n' || str[pos] == '\r')) {
    pos++;
  }
}

bool StubJsonDocument::parse_value(const std::string& json_str, size_t& pos,
                                   std::shared_ptr<StubJsonValue>& result) {
  skip_whitespace(json_str, pos);

  if(pos >= json_str.length()) {
    error_msg_ = "Unexpected end of input";
    return false;
  }

  char c = json_str[pos];

  if(c == '{') {
    return parse_object(json_str, pos, result);
  } else if(c == '[') {
    return parse_array(json_str, pos, result);
  } else if(c == '"') {
    return parse_string(json_str, pos, result);
  } else if(c == 't' || c == 'f') {
    return parse_bool(json_str, pos, result);
  } else if(c == 'n') {
    return parse_null(json_str, pos, result);
  } else if(c == '-' || (c >= '0' && c <= '9')) {
    return parse_number(json_str, pos, result);
  }

  error_msg_ = "Unexpected character: " + std::string(1, c);
  return false;
}

bool StubJsonDocument::parse_object(const std::string& json_str, size_t& pos,
                                    std::shared_ptr<StubJsonValue>& result) {
  result        = std::make_shared<StubJsonValue>();
  result->type_ = JsonValueType::JSON_OBJECT;

  pos++;  // Skip '{'
  skip_whitespace(json_str, pos);

  if(json_str[pos] == '}') {
    pos++;  // Empty object
    return true;
  }

  while(pos < json_str.length()) {
    skip_whitespace(json_str, pos);

    // Parse key (must be a string)
    std::shared_ptr<StubJsonValue> key_value;
    if(!parse_string(json_str, pos, key_value)) {
      return false;
    }

    std::string key = key_value->string_value_;

    skip_whitespace(json_str, pos);

    // Expect ':'
    if(json_str[pos] != ':') {
      error_msg_ = "Expected ':' after key";
      return false;
    }
    pos++;
    skip_whitespace(json_str, pos);

    // Parse value
    std::shared_ptr<StubJsonValue> value;
    if(!parse_value(json_str, pos, value)) {
      return false;
    }

    result->object_value_[key] = value;

    skip_whitespace(json_str, pos);

    // Expect ',' or '}'
    if(json_str[pos] == '}') {
      pos++;
      return true;
    } else if(json_str[pos] == ',') {
      pos++;
      continue;
    } else {
      error_msg_ = "Expected ',' or '}' in object";
      return false;
    }
  }

  error_msg_ = "Unexpected end of input in object";
  return false;
}

bool StubJsonDocument::parse_array(const std::string& json_str, size_t& pos,
                                   std::shared_ptr<StubJsonValue>& result) {
  result        = std::make_shared<StubJsonValue>();
  result->type_ = JsonValueType::JSON_ARRAY;

  pos++;  // Skip '['
  skip_whitespace(json_str, pos);

  if(json_str[pos] == ']') {
    pos++;  // Empty array
    return true;
  }

  while(pos < json_str.length()) {
    skip_whitespace(json_str, pos);

    // Parse value
    std::shared_ptr<StubJsonValue> value;
    if(!parse_value(json_str, pos, value)) {
      return false;
    }

    result->array_value_.push_back(value);

    skip_whitespace(json_str, pos);

    // Expect ',' or ']'
    if(json_str[pos] == ']') {
      pos++;
      return true;
    } else if(json_str[pos] == ',') {
      pos++;
      continue;
    } else {
      error_msg_ = "Expected ',' or ']' in array";
      return false;
    }
  }

  error_msg_ = "Unexpected end of input in array";
  return false;
}

bool StubJsonDocument::parse_string(const std::string& json_str, size_t& pos,
                                    std::shared_ptr<StubJsonValue>& result) {
  result        = std::make_shared<StubJsonValue>();
  result->type_ = JsonValueType::JSON_STRING;

  if(json_str[pos] != '"') {
    error_msg_ = "String must start with '\"'";
    return false;
  }
  pos++;  // Skip opening quote

  std::string value;
  while(pos < json_str.length()) {
    char c = json_str[pos++];

    if(c == '"') {
      result->string_value_ = value;
      return true;
    } else if(c == '\\') {
      // Handle escape sequences
      if(pos >= json_str.length()) {
        error_msg_ = "Unexpected end of input in string escape";
        return false;
      }
      char next = json_str[pos++];
      switch(next) {
        case '"':
          value += '"';
          break;
        case '\\':
          value += '\\';
          break;
        case '/':
          value += '/';
          break;
        case 'b':
          value += '\b';
          break;
        case 'f':
          value += '\f';
          break;
        case 'n':
          value += '\n';
          break;
        case 'r':
          value += '\r';
          break;
        case 't':
          value += '\t';
          break;
        case 'u': {
          // Unicode escape (simplified - just skip 4 hex digits)
          if(pos + 4 > json_str.length()) {
            error_msg_ = "Incomplete unicode escape";
            return false;
          }
          pos += 4;
          break;
        }
        default:
          error_msg_ = "Invalid escape sequence: \\" + std::string(1, next);
          return false;
      }
    } else {
      value += c;
    }
  }

  error_msg_ = "Unterminated string";
  return false;
}

bool StubJsonDocument::parse_bool(const std::string& json_str, size_t& pos,
                                  std::shared_ptr<StubJsonValue>& result) {
  result        = std::make_shared<StubJsonValue>();
  result->type_ = JsonValueType::JSON_BOOL;

  if(json_str[pos] == 't') {
    if(json_str.substr(pos, 4) == "true") {
      result->bool_value_ = true;
      pos += 4;
      return true;
    }
  } else if(json_str[pos] == 'f') {
    if(json_str.substr(pos, 5) == "false") {
      result->bool_value_ = false;
      pos += 5;
      return true;
    }
  }

  error_msg_ = "Invalid boolean value";
  return false;
}

bool StubJsonDocument::parse_null(const std::string& json_str, size_t& pos,
                                  std::shared_ptr<StubJsonValue>& result) {
  result        = std::make_shared<StubJsonValue>();
  result->type_ = JsonValueType::JSON_NULL;

  if(json_str.substr(pos, 4) == "null") {
    pos += 4;
    return true;
  }

  error_msg_ = "Invalid null value";
  return false;
}

bool StubJsonDocument::parse_number(const std::string& json_str, size_t& pos,
                                    std::shared_ptr<StubJsonValue>& result) {
  result        = std::make_shared<StubJsonValue>();
  result->type_ = JsonValueType::JSON_NUMBER;

  size_t start = pos;

  // Optional minus sign
  if(json_str[pos] == '-') {
    pos++;
  }

  // Integer part
  while(pos < json_str.length() && json_str[pos] >= '0' && json_str[pos] <= '9') {
    pos++;
  }

  // Fractional part
  if(pos < json_str.length() && json_str[pos] == '.') {
    pos++;
    while(pos < json_str.length() && json_str[pos] >= '0' && json_str[pos] <= '9') {
      pos++;
    }
  }

  // Exponent part
  if(pos < json_str.length() && (json_str[pos] == 'e' || json_str[pos] == 'E')) {
    pos++;
    if(pos < json_str.length() && (json_str[pos] == '+' || json_str[pos] == '-')) {
      pos++;
    }
    while(pos < json_str.length() && json_str[pos] >= '0' && json_str[pos] <= '9') {
      pos++;
    }
  }

  if(pos == start) {
    error_msg_ = "Invalid number";
    return false;
  }

  std::string num_str = json_str.substr(start, pos - start);
  try {
    if(num_str.find('.') != std::string::npos || num_str.find('e') != std::string::npos
       || num_str.find('E') != std::string::npos) {
      result->number_value_ = std::stod(num_str);
    } else {
      result->number_value_ = static_cast<double>(std::stoll(num_str));
    }
  } catch(const std::exception&) {
    error_msg_ = "Failed to parse number: " + num_str;
    return false;
  }

  return true;
}

}  // namespace dsl
}  // namespace agentflow

#endif  // !AGENTFLOW_WITH_RAPIDJSON
