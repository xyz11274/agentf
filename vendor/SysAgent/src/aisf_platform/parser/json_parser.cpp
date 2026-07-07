#include "aisf_platform/parser/json_parser.h"

namespace AisfPlat::JsonParser {
bool Parse(const string& data, Json::Value& root)
{
    CHECK_TRUE_RETURN(data.empty(), false);
    return Parse(data.c_str(), data.size(), root);
}

bool Parse(const char* data, Json::Value& root)
{
    CHECK_NULL_RETURN(data, false);
    return Parse(data, strlen(data), root);
}

bool Parse(const char* data, size_t size, Json::Value& root)
{
    CHECK_NULL_RETURN(data, false);
    CHECK_TRUE_RETURN(size == 0, false);

    Json::CharReaderBuilder builder;
    Json::CharReaderBuilder::strictMode(&builder.settings_); // 必须严格模式，不容错，如果容错，json格式错误，可能导致程序异常
    Json::CharReader* reader(builder.newCharReader());
    string errs;
    if (!reader->parse(data, data + size, &root, &errs) || root.isNull()) {
        LOG_INFO_EX("reader errs is:%s", errs.c_str());
        delete reader;
        return false;
    }
    delete reader;
    return true;
}

// JsonCpp使用了异常机制，但是华为不推荐使用异常，所以对于一些会抛出异常的函数进行简单封装，剔除异常
bool IsMember(const Json::Value& node, const string& member)
{
    try {
        return node.isMember(member); // isMember函数会抛出异常，导致进程复位，此异常必须捕获
    } catch (...) {
        return false;
    }
}

#define GET_NODE_VALUE(check, toValue, type)                                                         \
    do {                                                                                             \
        CHECK_TRUE_LOG_ERROR_RETURN(node.isNull(), false, "node is null");                           \
        CHECK_FALSE_RETURN(IsMember(node, member), false);                                           \
        if (node[member].check()) {                                                                  \
            value = node[member].toValue();                                                          \
            return true;                                                                             \
        }                                                                                            \
        LOG_ERR("member:%s type not %s", member.c_str(), type);                                      \
        return false;                                                                                \
    } while (0)

const Json::Value* GetNode(const Json::Value& node, const string& member)
{
    CHECK_TRUE_RETURN(node.isNull(), nullptr);
    CHECK_FALSE_RETURN(IsMember(node, member), nullptr);
    return &(node[member]);
}

bool GetInt(const Json::Value& node, const string& member, int& value)
{
    GET_NODE_VALUE(isInt, asInt, "int");
}

bool GetString(const Json::Value& node, const string& member, string& value)
{
    GET_NODE_VALUE(isString, asString, "string");
}

bool GetBool(const Json::Value& node, const string& member, bool& value)
{
    GET_NODE_VALUE(isBool, asBool, "bool");
}

bool GetInt64(const Json::Value& node, const string& member, long long int& value)
{
    GET_NODE_VALUE(isInt64, asInt64, "int64");
}

bool GetUInt(const Json::Value& node, const string& member, unsigned& value)
{
    GET_NODE_VALUE(isUInt, asUInt, "uint");
}

bool GetUInt64(const Json::Value& node, const string& member, unsigned long long& value)
{
    GET_NODE_VALUE(isUInt64, asUInt64, "uint64");
}

bool GetDouble(const Json::Value& node, const string& member, double& value)
{
    GET_NODE_VALUE(isDouble, asDouble, "double");
}

#define GET_NODE_ARRAY(check, toValue, type)                                                                \
    do {                                                                                                    \
        CHECK_TRUE_LOG_ERROR_RETURN(node.isNull(), false, "node is null");                                  \
        CHECK_FALSE_RETURN(IsMember(node, member), false);                                                  \
        CHECK_FALSE_LOG_ERROR_RETURN(node[member].isArray(), false, "member:%s not array", member.c_str()); \
        for (auto & nodeValue : node[member]) {                                                              \
            CHECK_FALSE_LOG_ERROR_RETURN(nodeValue.check(), false, "array type error not %s", type);        \
            value.push_back(nodeValue.toValue());                                                           \
        }                                                                                                   \
        return true;                                                                                        \
    } while (0)

#define GET_NODE_SET(check, toValue, type)                                                                \
    do {                                                                                                    \
        CHECK_TRUE_LOG_ERROR_RETURN(node.isNull(), false, "node is null");                                  \
        CHECK_FALSE_RETURN(IsMember(node, member), false);                                                  \
        CHECK_FALSE_LOG_ERROR_RETURN(node[member].isArray(), false, "member:%s not array", member.c_str()); \
        for (auto & nodeValue : node[member]) {                                                              \
            CHECK_FALSE_LOG_ERROR_RETURN(nodeValue.check(), false, "array type error not %s", type);        \
            value.insert(nodeValue.toValue());                                                           \
        }                                                                                                   \
        return true;                                                                                        \
    } while (0)

bool GetStringArray(const Json::Value& node, const string& member, vector<string>& value)
{
    GET_NODE_ARRAY(isString, asString, "string");
}

bool GetIntArray(const Json::Value& node, const string& member, vector<int>& value)
{
    GET_NODE_ARRAY(isInt, asInt, "int");
}

bool GetUIntArray(const Json::Value& node, const string& member, vector<unsigned>& value)
{
    GET_NODE_ARRAY(isUInt, asUInt, "uint");
}

bool GetUIntHashSet(const Json::Value& node, const string& member, unordered_set<unsigned>& value)
{
    GET_NODE_SET(isUInt, asUInt, "uint");
}

bool GetBoolArray(const Json::Value& node, const string& member, vector<bool>& value)
{
    GET_NODE_ARRAY(isBool, asBool, "bool");
}

bool GetInt64Array(const Json::Value& node, const string& member, vector<long long int>& value)
{
    GET_NODE_ARRAY(isInt64, asInt64, "int64");
}

bool GetUInt64Array(const Json::Value& node, const string& member, vector<unsigned long long>& value)
{
    GET_NODE_ARRAY(isUInt64, asUInt64, "uint64");
}

bool GetDoubleArray(const Json::Value& node, const string& member, vector<double>& value)
{
    GET_NODE_ARRAY(isDouble, asDouble, "double");
}

bool GetJsonValueArray(const Json::Value& node, const string& member, vector<const Json::Value*>& value)
{
    CHECK_TRUE_LOG_ERROR_RETURN(node.isNull(), false, "node is null");
    CHECK_FALSE_RETURN(IsMember(node, member), false);
    CHECK_FALSE_LOG_ERROR_RETURN(node[member].isArray(), false, "member:%s not array", member.c_str());
    for (const auto& nodeValue : node[member]) {
        value.push_back(&nodeValue);
    }

    return true;
}

string ToRawString(const Json::Value& root)
{
    Json::StreamWriterBuilder builder;
    builder["commentStyle"] = "None";
    builder["indentation"] = "";
    builder["emitUTF8"] = true;

    return Json::writeString(builder, root);
}

string ToPrettyString(const Json::Value& root)
{
    // 使用StreamWriterBuilder定制输出选项
    Json::StreamWriterBuilder writerBuilder;
    // 设置美化输出
    writerBuilder["indentation"] = "    ";
    // 重要：禁用Unicode转义，保持中文字符原样
    writerBuilder["emitUTF8"] = true;

    string formatted = Json::writeString(writerBuilder, root);
    return formatted;
}

string ToPrettyString(const string& jsonStr)
{
    Json::Value jsonValue;
    if (Parse(jsonStr, jsonValue)) {
        return ToPrettyString(jsonValue);
    }

    return jsonStr; // 解析失败时返回原始字符串
}

void ParseJsonToMap(const Json::Value& value, const std::string& prefix, std::map<std::string, std::string>& result) {
    if (value.isObject()) {
        for (const auto& key : value.getMemberNames()) {
            std::string currentPrefix = prefix.empty() ? key : prefix + "." + key;
            ParseJsonToMap(value[key], currentPrefix, result);
        }
    } else if (value.isArray()) {
        for (Json::ArrayIndex i = 0; i < value.size(); ++i) {
            std::string newPrefix = prefix + "[" + std::to_string(i) + "]";
            ParseJsonToMap(value[i], newPrefix, result);
        }
    } else if (value.isString()) {
        result[prefix] = value.asString();
    }
}

}