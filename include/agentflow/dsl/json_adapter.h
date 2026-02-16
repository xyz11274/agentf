// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// JSON processing adapter layer - independent of underlying JSON library
// Supports both rapidjson and stub implementations

#ifndef AGENTFLOW_DSL_JSON_ADAPTER_H
#define AGENTFLOW_DSL_JSON_ADAPTER_H

#include <memory>
#include <string>
#include <vector>
#include <map>
#include <variant>

namespace agentflow {
namespace dsl {

// Forward declarations
class IJsonValue;
class IJsonDocument;

/// @brief Generic JSON value type
enum class JsonValueType {
  JSON_NULL,
  JSON_BOOL,
  JSON_NUMBER,
  JSON_STRING,
  JSON_ARRAY,
  JSON_OBJECT,
};

/// @brief Generic JSON value interface
class IJsonValue {
 public:
  virtual ~IJsonValue() = default;

  /// @brief Get type of this value
  /// @return Type of value
  virtual JsonValueType get_type() const = 0;

  /// @brief Check if value is null
  virtual bool is_null() const = 0;

  /// @brief Check if value is boolean
  virtual bool is_bool() const = 0;

  /// @brief Check if value is number
  virtual bool is_number() const = 0;

  /// @brief Check if value is string
  virtual bool is_string() const = 0;

  /// @brief Check if value is array
  virtual bool is_array() const = 0;

  /// @brief Check if value is object
  virtual bool is_object() const = 0;

  // Type conversions
  virtual bool as_bool() const = 0;
  virtual double as_double() const = 0;
  virtual int64_t as_int64() const = 0;
  virtual uint64_t as_uint64() const = 0;
  virtual std::string as_string() const = 0;

  // Array operations
  /// @brief Get array size
  virtual size_t array_size() const = 0;

  /// @brief Get element at index
  virtual std::shared_ptr<IJsonValue> array_at(size_t index) = 0;

  /// @brief Get element at index (const version)
  virtual std::shared_ptr<const IJsonValue> array_at(size_t index) const = 0;

  // Object operations
  /// @brief Check if object has member
  virtual bool has_member(const std::string& key) const = 0;

  /// @brief Get object member
  virtual std::shared_ptr<IJsonValue> get_member(const std::string& key) = 0;

  /// @brief Get object member (const version)
  virtual std::shared_ptr<const IJsonValue> get_member(
      const std::string& key) const = 0;

  /// @brief Get all keys in object
  virtual std::vector<std::string> object_keys() const = 0;

  /// @brief Get object size
  virtual size_t object_size() const = 0;

  /// @brief Get raw JSON string representation
  virtual std::string to_string() const = 0;
};

/// @brief Generic JSON document interface
class IJsonDocument {
 public:
  virtual ~IJsonDocument() = default;

  /// @brief Parse JSON from string
  /// @param json_str JSON string to parse
  /// @return true if parsing successful
  virtual bool parse(const std::string& json_str) = 0;

  /// @brief Parse JSON from file
  /// @param file_path Path to JSON file
  /// @return true if parsing successful
  virtual bool parse_file(const std::string& file_path) = 0;

  /// @brief Get root value
  /// @return Root JSON value
  virtual std::shared_ptr<IJsonValue> root() = 0;

  /// @brief Get root value (const version)
  virtual std::shared_ptr<const IJsonValue> root() const = 0;

  /// @brief Get last error message
  /// @return Error message or empty string if no error
  virtual std::string get_error() const = 0;

  /// @brief Check if document is valid
  /// @return true if parsing was successful and no errors
  virtual bool is_valid() const = 0;

  /// @brief Get JSON string representation
  /// @return Pretty-printed JSON string
  virtual std::string to_string() const = 0;

  /// @brief Get compressed JSON string (no extra spaces)
  /// @return Compact JSON string
  virtual std::string to_compact_string() const = 0;
};

/// @brief Factory for creating JSON documents
class JsonDocumentFactory {
 public:
  /// @brief Create new JSON document
  /// @return Shared pointer to JSON document
  static std::shared_ptr<IJsonDocument> create();

  /// @brief Get implementation name
  /// @return Name of JSON library (e.g., "rapidjson", "stub")
  static std::string implementation_name();

  /// @brief Get implementation version
  /// @return Version string
  static std::string implementation_version();
};

/// @brief Utility helpers for JSON operations
class JsonHelper {
 public:
  /// @brief Safely get boolean value with default
  /// @param obj JSON object
  /// @param key Key to look up
  /// @param default_value Default if key not found or not bool
  /// @return Boolean value
  static bool get_bool(std::shared_ptr<const IJsonValue> obj,
                       const std::string& key,
                       bool default_value = false);

  /// @brief Safely get integer value with default
  /// @param obj JSON object
  /// @param key Key to look up
  /// @param default_value Default if key not found or not number
  /// @return Integer value
  static int64_t get_int(std::shared_ptr<const IJsonValue> obj,
                         const std::string& key,
                         int64_t default_value = 0);

  /// @brief Safely get double value with default
  /// @param obj JSON object
  /// @param key Key to look up
  /// @param default_value Default if key not found or not number
  /// @return Double value
  static double get_double(std::shared_ptr<const IJsonValue> obj,
                           const std::string& key,
                           double default_value = 0.0);

  /// @brief Safely get string value with default
  /// @param obj JSON object
  /// @param key Key to look up
  /// @param default_value Default if key not found or not string
  /// @return String value
  static std::string get_string(std::shared_ptr<const IJsonValue> obj,
                                const std::string& key,
                                const std::string& default_value = "");

  /// @brief Safely get object value
  /// @param obj JSON object
  /// @param key Key to look up
  /// @return Object if found and is object, nullptr otherwise
  static std::shared_ptr<const IJsonValue> get_object(
      std::shared_ptr<const IJsonValue> obj,
      const std::string& key);

  /// @brief Safely get array value
  /// @param obj JSON object
  /// @param key Key to look up
  /// @return Array if found and is array, nullptr otherwise
  static std::shared_ptr<const IJsonValue> get_array(
      std::shared_ptr<const IJsonValue> obj,
      const std::string& key);
};

}  // namespace dsl
}  // namespace agentflow

#endif  // AGENTFLOW_DSL_JSON_ADAPTER_H
