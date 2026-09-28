// 内部 JSON 类型：对网络/本地边界的数据提供确定性解析与类型检查。
// Protobuf 仍是持久化契约；公开头文件不必暴露内部 JSON 库。
// 缺失、null、空值和非法类型分别处理，避免默认值伪造不存在的观测。

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

  // 创建或读取受限错误信息，保留固定错误码而不泄露业务正文。
  Error(const char *code, int http = 400) : std::runtime_error(code), status(http) {}
};

// 统一检查前置条件；失败时抛出固定错误码，不把原始输入拼入诊断。
inline void require(bool value, const char *code, int status = 400) {
  if (!value)
    throw Error(code, status);
}

// 公开传输边界使用有界、确定性的 JSON。
// Protobuf 仍是轨迹和策略的持久契约，公共头文件不暴露 JSON 依赖。
class Json {
public:
  using Object = std::map<std::string, Json>;
  using Array = std::vector<Json>;
  std::variant<std::nullptr_t, bool, double, std::string, Object, Array> value;

  // 默认构造 JSON 空值；后续可写字段访问会把它提升为对象。
  Json() : value(nullptr) {}

  // 显式保存 JSON null，与字段不存在的判定分开。
  Json(std::nullptr_t) : value(nullptr) {}

  // 保存布尔值，不将 true/false 隐式转成数值。
  Json(bool v) : value(v) {}

  // 以统一的数值类型保存普通整数；读取整数时另外校验范围。
  Json(int v) : value(static_cast<double>(v)) {}

  // 把长度或计数写入 JSON 数值；调用方应遵守协议的精确整数范围。
  Json(std::size_t v) : value(static_cast<double>(v)) {}

  // 保存浮点数；具体评分和配置边界的有限值检查由使用处负责。
  Json(double v) : value(v) {}

  // 复制以零结尾的字符串，不借用调用方缓冲区。
  Json(const char *v) : value(std::string(v)) {}

  // 接管字符串值，之后不依赖传入对象的生命周期。
  Json(std::string v) : value(std::move(v)) {}

  // 接管对象字段集合，字段名按 map 的稳定顺序组织。
  Json(Object v) : value(std::move(v)) {}

  // 接管数组元素，保留调用方提供的元素顺序。
  Json(Array v) : value(std::move(v)) {}

  // 从键值列表构造对象，供协议处理处声明小型响应和配置。
  Json(std::initializer_list<std::pair<const std::string, Json>> v) : value(Object(v)) {}

  // 检查当前 JSON 是否为空值，空值与字段缺失由调用方区分。
  bool null() const {
    return std::holds_alternative<std::nullptr_t>(value);
  }

  // 检查当前 JSON 是否为对象，不做隐式类型转换。
  bool object() const {
    return std::holds_alternative<Object>(value);
  }

  // 检查当前 JSON 是否为数组，供后续安全遍历。
  bool array() const {
    return std::holds_alternative<Array>(value);
  }

  // 读取或检查字符串表示，保持接口要求的类型和编码约束。
  bool string() const {
    return std::holds_alternative<std::string>(value);
  }

  // 验证或读取当前对象的字段集合，拒绝与协议不符的结构。
  Object &fields() {
    require(object(), "object_required");
    return std::get<Object>(value);
  }

  // 验证或读取当前对象的字段集合，拒绝与协议不符的结构。
  const Object &fields() const {
    require(object(), "object_required");
    return std::get<Object>(value);
  }

  // 返回经过类型检查的数组元素集合，非数组不能直接迭代。
  Array &items() {
    require(array(), "array_required");
    return std::get<Array>(value);
  }

  // 返回经过类型检查的数组元素集合，非数组不能直接迭代。
  const Array &items() const {
    require(array(), "array_required");
    return std::get<Array>(value);
  }

  // 读取经过类型检查的字符串值，拒绝其他类型隐式转换。
  std::string text() const {
    require(string(), "string_required");
    return std::get<std::string>(value);
  }

  // 读取明确布尔值，避免将非空字符串误判为真。
  bool boolean() const {
    require(std::holds_alternative<bool>(value), "boolean_required");
    return std::get<bool>(value);
  }

  // 读取非负整数并检查范围，拒绝浮点小数和溢出。
  std::size_t integer() const {
    require(std::holds_alternative<double>(value), "integer_required");
    const double v = std::get<double>(value);
    require(std::isfinite(v) && v >= 0 && v <= 9007199254740991.0 && std::floor(v) == v,
            "integer_invalid");
    return static_cast<std::size_t>(v);
  }

  // 按字段名访问 JSON 成员；可写访问与只读缺失值遵循本类约定。
  Json &operator[](const std::string &key) {
    if (null())
      value = Object{};
    return fields()[key];
  }

  // 按字段名访问 JSON 成员；可写访问与只读缺失值遵循本类约定。
  const Json &operator[](const std::string &key) const {
    static const Json empty;
    if (!object())
      return empty;
    auto it = fields().find(key);
    return it == fields().end() ? empty : it->second;
  }

  // 检查字段是否存在，不把显式 null 自动当作字段缺失。
  bool has(const std::string &key) const {
    return object() && fields().count(key);
  }

  // 读取字符串成员或指定缺省值，保持类型错误可见。
  std::string str(const std::string &key, const std::string &fallback = {}) const {
    return has(key) ? (*this)[key].text() : fallback;
  }

  // 读取数值成员或指定缺省值，并验证整数范围。
  std::size_t num(const std::string &key, std::size_t fallback) const {
    return has(key) ? (*this)[key].integer() : fallback;
  }

  // 读取布尔成员或指定缺省值，不接受字符串真假。
  bool flag(const std::string &key, bool fallback) const {
    return has(key) ? (*this)[key].boolean() : fallback;
  }

  // 向数组追加元素；初始 null 自动提升为空数组，其他非数组类型仍拒绝。
  void push(Json v) {
    if (null())
      value = Array{};
    items().push_back(std::move(v));
  }

  // 把解析库中的值递归转换为内部 JSON 类型，保留原始类型与结构。
  static Json convert(const rapidjson::Value &v, std::size_t depth = 0) {
    require(depth <= 48, "json_too_deep");
    if (v.IsObject()) {
      Object out;
      for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
        std::string key(it->name.GetString(), it->name.GetStringLength());
        require(out.emplace(key, convert(it->value, depth + 1)).second, "duplicate_json_key");
      }
      return out;
    }
    if (v.IsArray()) {
      Array out;
      for (const auto &x : v.GetArray())
        out.push_back(convert(x, depth + 1));
      return out;
    }
    if (v.IsString())
      return std::string(v.GetString(), v.GetStringLength());
    if (v.IsBool())
      return v.GetBool();
    if (v.IsNumber()) {
      require(std::isfinite(v.GetDouble()), "number_invalid");
      require(!(v.IsInt64() || v.IsUint64()) || std::abs(v.GetDouble()) <= 9007199254740991.0,
              "large_integer_requires_string");
      return v.GetDouble();
    }
    return nullptr;
  }

  // 解析有界 JSON 并检查格式及重复字段，拒绝歧义输入。
  static Json parse(const std::string &bytes) {
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(bytes.data(),
                                                                                      bytes.size());
    require(!doc.HasParseError(), "invalid_json");
    return convert(doc);
  }

  template <class Writer> void write(Writer &w, bool transport = false) const {
    if (null()) {
      w.Null();
      return;
    }
    if (std::holds_alternative<bool>(value)) {
      w.Bool(std::get<bool>(value));
      return;
    }
    if (std::holds_alternative<double>(value)) {
      const auto number = std::get<double>(value);
      if (transport && std::abs(number) <= 9007199254740991.0 && std::floor(number) == number &&
          !(number == 0 && std::signbit(number)))
        w.Int64(static_cast<std::int64_t>(number));
      else
        w.Double(number);
      return;
    }
    if (string()) {
      const auto &s = std::get<std::string>(value);
      w.String(s.data(), s.size());
      return;
    }
    if (array()) {
      w.StartArray();
      for (const auto &v : items())
        v.write(w, transport);
      w.EndArray();
      return;
    }
    w.StartObject();
    for (const auto &item : fields()) {
      w.Key(item.first.data(), item.first.size());
      item.second.write(w, transport);
    }
    w.EndObject();
  }

  // 序列化当前对象为紧凑 JSON，供协议传输或持久化使用。
  std::string dump() const {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
    write(w);
    return {buffer.GetString(), buffer.GetSize()};
  }

  // 宿主请求需要整数 JSON 表示；存储摘要仍保持原规范编码，
  // 确保既有轨迹和策略版本继续有效。
  std::string dump_for_transport() const {
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> w(buffer);
    write(w, true);
    return {buffer.GetString(), buffer.GetSize()};
  }
};

template <class Message> Message proto(const Json &json) {
  Message out;
  require(google::protobuf::util::JsonStringToMessage(json.dump(), &out).ok(), "contract_invalid");
  return out;
}

template <class Message> Json json(const Message &message) {
  google::protobuf::util::JsonPrintOptions options;
  options.preserve_proto_field_names = true;
  std::string bytes;
  require(google::protobuf::util::MessageToJsonString(message, &bytes, options).ok(),
          "serialization_failed", 500);
  return Json::parse(bytes);
}

// 校验对象只含允许字段，阻止拼写错误或额外字段绕过协议。
inline void keys(const Json &object, std::initializer_list<const char *> allowed) {
  require(object.object(), "object_required");
  for (const auto &pair : object.fields()) {
    bool found = false;
    for (auto key : allowed)
      found |= pair.first == key;
    require(found, "unknown_field");
  }
}

// 检查标识符是否符合协议允许的字符和长度，拒绝把任意路径当作身份使用。
inline bool identifier(const std::string &value) {
  return !value.empty() && value.size() <= 128 &&
         std::all_of(value.begin(), value.end(),
                     [](unsigned char c) { return c >= 32 && c != 127; });
}
} // 命名空间结束：trajectory::evolution::integration
