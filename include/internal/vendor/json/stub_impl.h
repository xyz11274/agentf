// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// Stub JSON implementation (when rapidjson is not available)

#ifndef AGENTFLOW_DSL_JSON_ADAPTER_STUB_IMPL_H
#define AGENTFLOW_DSL_JSON_ADAPTER_STUB_IMPL_H

#ifndef AGENTFLOW_WITH_RAPIDJSON

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "workflow/dsl/json_adapter.h"

namespace agentflow {
namespace dsl {

// Forward declaration
class StubJsonDocument;

/// @brief Simple value holder for stub JSON implementation
class StubJsonValue : public IJsonValue {
  friend class StubJsonDocument;  // Allow document to access private members
public:
  StubJsonValue() = default;
  explicit StubJsonValue(const std::string& string_value);
  explicit StubJsonValue(double number_value);
  explicit StubJsonValue(bool bool_value);
  ~StubJsonValue() override;

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

  // Setters for building JSON
  void add_array_element(std::shared_ptr<IJsonValue> value);
  void set_object_member(const std::string& key, std::shared_ptr<IJsonValue> value);

private:
  JsonValueType                                      type_ = JsonValueType::JSON_NULL;
  std::string                                        string_value_;
  double                                             number_value_ = 0.0;
  bool                                               bool_value_   = false;
  std::vector<std::shared_ptr<IJsonValue>>           array_value_;
  std::map<std::string, std::shared_ptr<IJsonValue>> object_value_;
};

/// @brief Stub implementation of JSON document
class StubJsonDocument : public IJsonDocument {
public:
  StubJsonDocument();
  ~StubJsonDocument() override;

  bool parse(const std::string& json_str) override;
  bool parse_file(const std::string& file_path) override;

  std::shared_ptr<IJsonValue>       root() override;
  std::shared_ptr<const IJsonValue> root() const override;

  std::string get_error() const override;
  bool        is_valid() const override;

  std::string to_string() const override;
  std::string to_compact_string() const override;

private:
  std::shared_ptr<StubJsonValue> root_;
  std::string                    error_msg_;
  bool                           valid_ = false;

  // Simple JSON parser for stub implementation
  bool parse_value(const std::string& json_str, size_t& pos,
                   std::shared_ptr<StubJsonValue>& result);
  bool parse_object(const std::string& json_str, size_t& pos,
                    std::shared_ptr<StubJsonValue>& result);
  bool parse_array(const std::string& json_str, size_t& pos,
                   std::shared_ptr<StubJsonValue>& result);
  bool parse_string(const std::string& json_str, size_t& pos,
                    std::shared_ptr<StubJsonValue>& result);
  bool parse_bool(const std::string& json_str, size_t& pos, std::shared_ptr<StubJsonValue>& result);
  bool parse_null(const std::string& json_str, size_t& pos, std::shared_ptr<StubJsonValue>& result);
  bool parse_number(const std::string& json_str, size_t& pos,
                    std::shared_ptr<StubJsonValue>& result);
  void skip_whitespace(const std::string& str, size_t& pos);
};

}  // namespace dsl
}  // namespace agentflow

#endif  // AGENTFLOW_WITH_RAPIDJSON

#endif  // AGENTFLOW_DSL_JSON_ADAPTER_STUB_IMPL_H
