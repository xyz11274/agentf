// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Stub JSON adapter implementation for testing without rapidjson

#ifndef AGENTFLOW_WITH_RAPIDJSON

#include "agentflow/dsl/json_adapter/stub_impl.h"
#include <sstream>

namespace agentflow {
namespace dsl {

// StubJsonValue implementation

StubJsonValue::StubJsonValue(const std::string& string_value) 
    : type_(JsonValueType::JSON_STRING), string_value_(string_value) {}

StubJsonValue::StubJsonValue(double number_value) 
    : type_(JsonValueType::JSON_NUMBER), number_value_(number_value) {}

StubJsonValue::StubJsonValue(bool bool_value) 
    : type_(JsonValueType::JSON_BOOL), bool_value_(bool_value) {}

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
  if (index >= array_value_.size()) {
    return nullptr;
  }
  return array_value_[index];
}

std::shared_ptr<const IJsonValue> StubJsonValue::array_at(size_t index) const {
  if (index >= array_value_.size()) {
    return nullptr;
  }
  return array_value_[index];
}

bool StubJsonValue::has_member(const std::string& key) const {
  return object_value_.find(key) != object_value_.end();
}

std::shared_ptr<IJsonValue> StubJsonValue::get_member(
    const std::string& key) {
  auto it = object_value_.find(key);
  if (it == object_value_.end()) {
    return nullptr;
  }
  return it->second;
}

std::shared_ptr<const IJsonValue> StubJsonValue::get_member(
    const std::string& key) const {
  auto it = object_value_.find(key);
  if (it == object_value_.end()) {
    return nullptr;
  }
  return it->second;
}

std::vector<std::string> StubJsonValue::object_keys() const {
  std::vector<std::string> keys;
  for (const auto& pair : object_value_) {
    keys.push_back(pair.first);
  }
  return keys;
}

size_t StubJsonValue::object_size() const {
  return object_value_.size();
}

std::string StubJsonValue::to_string() const {
  std::stringstream ss;
  switch (type_) {
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

void StubJsonValue::set_object_member(const std::string& key,
                                      std::shared_ptr<IJsonValue> value) {
  type_ = JsonValueType::JSON_OBJECT;
  object_value_[key] = value;
}

// StubJsonDocument implementation

StubJsonDocument::StubJsonDocument() = default;

StubJsonDocument::~StubJsonDocument() = default;

bool StubJsonDocument::parse(const std::string& json_str) {
  // Stub implementation: always succeeds with empty root
  valid_ = true;
  error_msg_ = "";
  root_ = std::make_shared<StubJsonValue>();
  return true;
}

bool StubJsonDocument::parse_file(const std::string& file_path) {
  // Stub implementation: always succeeds
  valid_ = true;
  error_msg_ = "";
  root_ = std::make_shared<StubJsonValue>();
  return true;
}

std::shared_ptr<IJsonValue> StubJsonDocument::root() {
  if (!root_) {
    root_ = std::make_shared<StubJsonValue>();
  }
  return root_;
}

std::shared_ptr<const IJsonValue> StubJsonDocument::root() const {
  if (!root_) {
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
  if (root_) {
    return root_->to_string();
  }
  return "{}";
}

std::string StubJsonDocument::to_compact_string() const {
  return to_string();
}

}  // namespace dsl
}  // namespace agentflow

#endif  // !AGENTFLOW_WITH_RAPIDJSON
