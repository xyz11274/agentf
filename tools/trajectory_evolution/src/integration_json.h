#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <google/protobuf/util/json_util.h>
#include "agentflow/trajectory/hybrid_evaluator.h"

namespace trajectory::evolution::integration {

struct Error : std::runtime_error {
  int status;
  Error(const char* code, int http = 400) : std::runtime_error(code), status(http) {}
};
inline void require(bool value, const char* code, int status = 400) {
  if(!value) throw Error(code, status);
}

// Bounded, deterministic JSON at the public transport boundary. Protobuf remains
// the durable trajectory and policy contract; no JSON dependency in public headers.
class Json {
 public:
  using Object = std::map<std::string, Json>;
  using Array = std::vector<Json>;
  std::variant<std::nullptr_t, bool, double, std::string, Object, Array> value;
  Json() : value(nullptr) {}
  Json(std::nullptr_t) : value(nullptr) {}
  Json(bool v) : value(v) {}
  Json(int v) : value(static_cast<double>(v)) {}
  Json(std::size_t v) : value(static_cast<double>(v)) {}
  Json(double v) : value(v) {}
  Json(const char* v) : value(std::string(v)) {}
  Json(std::string v) : value(std::move(v)) {}
  Json(Object v) : value(std::move(v)) {}
  Json(Array v) : value(std::move(v)) {}
  Json(std::initializer_list<std::pair<const std::string, Json>> v) : value(Object(v)) {}
  bool null() const { return std::holds_alternative<std::nullptr_t>(value); }
  bool object() const { return std::holds_alternative<Object>(value); }
  bool array() const { return std::holds_alternative<Array>(value); }
  bool string() const { return std::holds_alternative<std::string>(value); }
  Object& fields() { require(object(), "object_required"); return std::get<Object>(value); }
  const Object& fields() const { require(object(), "object_required"); return std::get<Object>(value); }
  Array& items() { require(array(), "array_required"); return std::get<Array>(value); }
  const Array& items() const { require(array(), "array_required"); return std::get<Array>(value); }
  std::string text() const { require(string(), "string_required"); return std::get<std::string>(value); }
  bool boolean() const { require(std::holds_alternative<bool>(value), "boolean_required"); return std::get<bool>(value); }
  std::size_t integer() const {
    require(std::holds_alternative<double>(value), "integer_required");
    const double v = std::get<double>(value);
    require(std::isfinite(v) && v >= 0 && v <= 9007199254740991.0 && std::floor(v) == v, "integer_invalid");
    return static_cast<std::size_t>(v);
  }
  Json& operator[](const std::string& key) { if(null()) value = Object{}; return fields()[key]; }
  const Json& operator[](const std::string& key) const {
    static const Json empty;
    if(!object()) return empty;
    auto it = fields().find(key); return it == fields().end() ? empty : it->second;
  }
  bool has(const std::string& key) const { return object() && fields().count(key); }
  std::string str(const std::string& key, const std::string& fallback = {}) const {
    return has(key) ? (*this)[key].text() : fallback;
  }
  std::size_t num(const std::string& key, std::size_t fallback) const {
    return has(key) ? (*this)[key].integer() : fallback;
  }
  bool flag(const std::string& key, bool fallback) const {
    return has(key) ? (*this)[key].boolean() : fallback;
  }
  void push(Json v) { if(null()) value = Array{}; items().push_back(std::move(v)); }
  static Json convert(const rapidjson::Value& v, std::size_t depth = 0) {
    require(depth <= 48, "json_too_deep");
    if(v.IsObject()) {
      Object out;
      for(auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
        std::string key(it->name.GetString(), it->name.GetStringLength());
        require(out.emplace(key, convert(it->value, depth + 1)).second, "duplicate_json_key");
      }
      return out;
    }
    if(v.IsArray()) { Array out; for(const auto& x : v.GetArray()) out.push_back(convert(x, depth + 1)); return out; }
    if(v.IsString()) return std::string(v.GetString(), v.GetStringLength());
    if(v.IsBool()) return v.GetBool();
    if(v.IsNumber()) {
      require(std::isfinite(v.GetDouble()), "number_invalid");
      require(!(v.IsInt64() || v.IsUint64()) || std::abs(v.GetDouble()) <= 9007199254740991.0, "large_integer_requires_string");
      return v.GetDouble();
    }
    return nullptr;
  }
  static Json parse(const std::string& bytes) {
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(bytes.data(), bytes.size());
    require(!doc.HasParseError(), "invalid_json");
    return convert(doc);
  }
  template<class Writer> void write(Writer& w, bool transport = false) const {
    if(null()) { w.Null(); return; }
    if(std::holds_alternative<bool>(value)) { w.Bool(std::get<bool>(value)); return; }
    if(std::holds_alternative<double>(value)) {
      const auto number = std::get<double>(value);
      if(transport && std::abs(number) <= 9007199254740991.0 && std::floor(number) == number
          && !(number == 0 && std::signbit(number))) w.Int64(static_cast<std::int64_t>(number));
      else w.Double(number);
      return;
    }
    if(string()) { const auto& s = std::get<std::string>(value); w.String(s.data(), s.size()); return; }
    if(array()) { w.StartArray(); for(const auto& v : items()) v.write(w, transport); w.EndArray(); return; }
    w.StartObject();
    for(const auto& item : fields()) { w.Key(item.first.data(), item.first.size()); item.second.write(w, transport); }
    w.EndObject();
  }
  std::string dump() const { rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> w(buffer); write(w); return {buffer.GetString(), buffer.GetSize()}; }
  // Host requests need integer JSON tokens; stored hashes keep the original
  // canonical encoding so existing traces and policy versions remain valid.
  std::string dump_for_transport() const {
    rapidjson::StringBuffer buffer; rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
    write(w, true); return {buffer.GetString(), buffer.GetSize()};
  }
};

template<class Message> Message proto(const Json& json) {
  Message out;
  require(google::protobuf::util::JsonStringToMessage(json.dump(), &out).ok(), "contract_invalid");
  return out;
}
template<class Message> Json json(const Message& message) {
  google::protobuf::util::JsonPrintOptions options; options.preserve_proto_field_names = true;
  std::string bytes;
  require(google::protobuf::util::MessageToJsonString(message, &bytes, options).ok(), "serialization_failed", 500);
  return Json::parse(bytes);
}
inline void keys(const Json& object, std::initializer_list<const char*> allowed) {
  require(object.object(), "object_required");
  for(const auto& pair : object.fields()) {
    bool found = false; for(auto key : allowed) found |= pair.first == key;
    require(found, "unknown_field");
  }
}
inline bool identifier(const std::string& value) {
  return !value.empty() && value.size() <= 128 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return c >= 32 && c != 127;
  });
}
}  // namespace trajectory::evolution::integration
