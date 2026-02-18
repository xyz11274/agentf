// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Rapidjson implementation of JSON adapter

#ifndef AGENTFLOW_DSL_JSON_ADAPTER_RAPIDJSON_IMPL_H
#define AGENTFLOW_DSL_JSON_ADAPTER_RAPIDJSON_IMPL_H

#ifdef AGENTFLOW_WITH_RAPIDJSON

#include <memory>
#include <rapidjson/document.h>
#include <string>

#include "workflow/json_adapter.h"

namespace agentflow {
namespace dsl {

/// @brief Rapidjson implementation of IJsonValue
class RapidjsonValue : public IJsonValue {
public:
  /// @brief Constructor taking rapidjson Value
  /// @param value Pointer to rapidjson Value
  /// @param doc Pointer to parent document (for memory management)
  RapidjsonValue(rapidjson::Value* value, rapidjson::Document* doc);

  ~RapidjsonValue() override;

  JsonValueType get_type() const override;
  bool          is_null() const override;
  bool          is_bool() const override;
  bool          is_number() const override;
  bool          is_string() const override;
  bool          is_array() const override;
  bool          is_object() const override;

  bool        as_bool() const override;
  double      as_double() const override;
  int64_t     as_int64() const override;
  uint64_t    as_uint64() const override;
  std::string as_string() const override;

  size_t                            array_size() const override;
  std::shared_ptr<IJsonValue>       array_at(size_t index) override;
  std::shared_ptr<const IJsonValue> array_at(size_t index) const override;

  bool                              has_member(const std::string& key) const override;
  std::shared_ptr<IJsonValue>       get_member(const std::string& key) override;
  std::shared_ptr<const IJsonValue> get_member(const std::string& key) const override;
  std::vector<std::string>          object_keys() const override;
  size_t                            object_size() const override;

  std::string to_string() const override;

  /// @brief Get underlying rapidjson Value
  rapidjson::Value* get_raw_value() { return value_; }

private:
  rapidjson::Value*    value_;
  rapidjson::Document* doc_;
};

/// @brief Rapidjson implementation of IJsonDocument
class RapidjsonDocument : public IJsonDocument {
public:
  RapidjsonDocument();
  ~RapidjsonDocument() override;

  bool parse(const std::string& json_str) override;
  bool parse_file(const std::string& file_path) override;

  std::shared_ptr<IJsonValue>       root() override;
  std::shared_ptr<const IJsonValue> root() const override;

  std::string get_error() const override;
  bool        is_valid() const override;

  std::string to_string() const override;
  std::string to_compact_string() const override;

  /// @brief Get underlying rapidjson Document
  rapidjson::Document* get_raw_document() { return doc_.get(); }

private:
  std::unique_ptr<rapidjson::Document> doc_;
  std::string                          error_msg_;
  bool                                 valid_ = false;

  friend class RapidjsonValue;
};

}  // namespace dsl
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_RAPIDJSON

#endif  // AGENTFLOW_DSL_JSON_ADAPTER_RAPIDJSON_IMPL_H
