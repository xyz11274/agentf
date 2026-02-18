// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Rapidjson implementation

#ifdef AGENTFLOW_WITH_RAPIDJSON

#include <fstream>
#include <sstream>

#include "dsl/json_adapter/rapidjson_impl.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace agentflow {
namespace dsl {

RapidjsonValue::RapidjsonValue(rapidjson::Value* value, rapidjson::Document* doc)
    : value_(value), doc_(doc) {
}

RapidjsonValue::~RapidjsonValue() = default;

JsonValueType RapidjsonValue::get_type() const {
  if(!value_)
    return JsonValueType::JSON_NULL;

  if(value_->IsNull())
    return JsonValueType::JSON_NULL;
  if(value_->IsBool())
    return JsonValueType::JSON_BOOL;
  if(value_->IsNumber())
    return JsonValueType::JSON_NUMBER;
  if(value_->IsString())
    return JsonValueType::JSON_STRING;
  if(value_->IsArray())
    return JsonValueType::JSON_ARRAY;
  if(value_->IsObject())
    return JsonValueType::JSON_OBJECT;

  return JsonValueType::JSON_NULL;
}

bool RapidjsonValue::is_null() const {
  return value_ && value_->IsNull();
}

bool RapidjsonValue::is_bool() const {
  return value_ && value_->IsBool();
}

bool RapidjsonValue::is_number() const {
  return value_ && value_->IsNumber();
}

bool RapidjsonValue::is_string() const {
  return value_ && value_->IsString();
}

bool RapidjsonValue::is_array() const {
  return value_ && value_->IsArray();
}

bool RapidjsonValue::is_object() const {
  return value_ && value_->IsObject();
}

bool RapidjsonValue::as_bool() const {
  if(!value_ || !value_->IsBool())
    return false;
  return value_->GetBool();
}

double RapidjsonValue::as_double() const {
  if(!value_ || !value_->IsNumber())
    return 0.0;
  return value_->GetDouble();
}

int64_t RapidjsonValue::as_int64() const {
  if(!value_ || !value_->IsNumber())
    return 0;
  return value_->GetInt64();
}

uint64_t RapidjsonValue::as_uint64() const {
  if(!value_ || !value_->IsNumber())
    return 0;
  return value_->GetUint64();
}

std::string RapidjsonValue::as_string() const {
  if(!value_ || !value_->IsString())
    return "";
  return std::string(value_->GetString(), value_->GetStringLength());
}

size_t RapidjsonValue::array_size() const {
  if(!value_ || !value_->IsArray())
    return 0;
  return value_->Size();
}

std::shared_ptr<IJsonValue> RapidjsonValue::array_at(size_t index) {
  if(!value_ || !value_->IsArray() || index >= value_->Size()) {
    return nullptr;
  }
  return std::make_shared<RapidjsonValue>(&(*value_)[index], doc_);
}

std::shared_ptr<const IJsonValue> RapidjsonValue::array_at(size_t index) const {
  if(!value_ || !value_->IsArray() || index >= value_->Size()) {
    return nullptr;
  }
  return std::make_shared<RapidjsonValue>(const_cast<rapidjson::Value*>(&(*value_)[index]), doc_);
}

bool RapidjsonValue::has_member(const std::string& key) const {
  if(!value_ || !value_->IsObject())
    return false;
  return value_->HasMember(key.c_str());
}

std::shared_ptr<IJsonValue> RapidjsonValue::get_member(const std::string& key) {
  if(!value_ || !value_->IsObject() || !value_->HasMember(key.c_str())) {
    return nullptr;
  }
  return std::make_shared<RapidjsonValue>(&(*value_)[key.c_str()], doc_);
}

std::shared_ptr<const IJsonValue> RapidjsonValue::get_member(const std::string& key) const {
  if(!value_ || !value_->IsObject() || !value_->HasMember(key.c_str())) {
    return nullptr;
  }
  return std::make_shared<RapidjsonValue>(const_cast<rapidjson::Value*>(&(*value_)[key.c_str()]),
                                          doc_);
}

std::vector<std::string> RapidjsonValue::object_keys() const {
  std::vector<std::string> keys;
  if(!value_ || !value_->IsObject())
    return keys;

  for(auto& member : value_->GetObject()) {
    keys.push_back(member.name.GetString());
  }
  return keys;
}

size_t RapidjsonValue::object_size() const {
  if(!value_ || !value_->IsObject())
    return 0;
  return value_->Size();
}

std::string RapidjsonValue::to_string() const {
  if(!value_)
    return "";

  rapidjson::StringBuffer                    buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  value_->Accept(writer);
  return buffer.GetString();
}

// RapidjsonDocument implementation

RapidjsonDocument::RapidjsonDocument() : doc_(std::make_unique<rapidjson::Document>()) {
}

RapidjsonDocument::~RapidjsonDocument() = default;

bool RapidjsonDocument::parse(const std::string& json_str) {
  if(!doc_) {
    error_msg_ = "Document not initialized";
    return false;
  }

  doc_->Parse(json_str.c_str());

  if(doc_->HasParseError()) {
    std::stringstream ss;
    ss << "Parse error at offset " << doc_->GetErrorOffset() << ": " << doc_->GetParseError();
    error_msg_ = ss.str();
    valid_     = false;
    return false;
  }

  valid_     = true;
  error_msg_ = "";
  return true;
}

bool RapidjsonDocument::parse_file(const std::string& file_path) {
  std::ifstream file(file_path);
  if(!file.is_open()) {
    error_msg_ = "Cannot open file: " + file_path;
    return false;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  file.close();

  return parse(buffer.str());
}

std::shared_ptr<IJsonValue> RapidjsonDocument::root() {
  if(!valid_ || !doc_)
    return nullptr;
  return std::make_shared<RapidjsonValue>(doc_.get(), doc_.get());
}

std::shared_ptr<const IJsonValue> RapidjsonDocument::root() const {
  if(!valid_ || !doc_)
    return nullptr;
  return std::make_shared<RapidjsonValue>(const_cast<rapidjson::Document*>(doc_.get()),
                                          const_cast<rapidjson::Document*>(doc_.get()));
}

std::string RapidjsonDocument::get_error() const {
  return error_msg_;
}

bool RapidjsonDocument::is_valid() const {
  return valid_;
}

std::string RapidjsonDocument::to_string() const {
  if(!valid_ || !doc_)
    return "";

  rapidjson::StringBuffer                    buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc_->Accept(writer);
  return buffer.GetString();
}

std::string RapidjsonDocument::to_compact_string() const {
  return to_string();  // Rapidjson writer produces compact format by default
}

}  // namespace dsl
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_RAPIDJSON