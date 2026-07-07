#pragma once

#include "aisf_platform/common_define.h"
#include "json/json.h"

namespace AisfPlat {
using std::string;
using std::vector;

namespace JsonParser {
[[nodiscard]]  bool Parse(const string& data, Json::Value& root);

[[nodiscard]]  bool Parse(const char* data, Json::Value& root);

[[nodiscard]]   bool Parse(const char* data, size_t size, Json::Value& root);

bool IsMember(const Json::Value& node, const string& member);

const Json::Value* GetNode(const Json::Value& node, const string& member);

bool GetInt(const Json::Value& node, const string& member, int& value);

bool GetString(const Json::Value& node, const string& member, string& value);

bool GetBool(const Json::Value& node, const string& member, bool& value);

bool GetInt64(const Json::Value& node, const string& member, long long int& value);

bool GetUInt(const Json::Value& node, const string& member, unsigned& value);

bool GetUInt64(const Json::Value& node, const string& member, unsigned long long& value);

bool GetDouble(const Json::Value& node, const string& member, double& value);

bool GetIntArray(const Json::Value& node, const string& member, vector<int>& value);

bool GetUIntArray(const Json::Value& node, const string& member, vector<unsigned>& value);

bool GetUIntHashSet(const Json::Value& node, const string& member, std::unordered_set<unsigned>& value);

bool GetStringArray(const Json::Value& node, const string& member, vector<string>& value);

bool GetBoolArray(const Json::Value& node, const string& member, vector<bool>& value);

bool GetInt64Array(const Json::Value& node, const string& member, vector<long long int>& value);

bool GetUInt64Array(const Json::Value& node, const string& member, vector<unsigned long long>& value);

bool GetDoubleArray(const Json::Value& node, const string& member, vector<double>& value);

bool GetJsonValueArray(const Json::Value& node, const string& member, vector<const Json::Value*>& value);

void ParseJsonToMap(const Json::Value& value, const std::string& prefix, std::map<std::string, std::string>& result);

// 将Json对象输出为不带格式的字符串，人眼阅读困难，但是节省内存、CPU、网络带宽等资源
[[nodiscard]] string ToRawString(const Json::Value& root);

[[nodiscard]] string ToPrettyString(const Json::Value& root);

[[nodiscard]] string ToPrettyString(const string& jsonStr);
}
}
