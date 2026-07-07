//
// Created by l00959838 on 26-2-13.
//

#ifndef SYSAGENT_JSON_CODER_H
#define SYSAGENT_JSON_CODER_H
#include <string>
#include "json/json.h"
#include "framework_common/logger/logger.h"

namespace SYSAGENT {

    constexpr int JSON_MAX_LEN = 8 * 1024 * 1024;

    class JsonCoder {
    public:
        static bool Parse(const char *data, Json::Value& root);

        static bool ParseFromFile(const std::string& filePath, Json::Value& root);

        static bool IsMember(const Json::Value& node, const std::string& key);

        static const Json::Value GetNode(const Json::Value &node, const std::string &member);

        template<class T>
        static bool GetNodeValue(const Json::Value &node, const std::string &member, T &value);

        static std::string Write(const Json::Value &root);

        static std::string WriteWithIndentation(const Json::Value &root);
    };

    template<class T>
    bool JsonCoder::GetNodeValue(const Json::Value &node, const std::string &member, T &value)
    {
        if (node.isNull() || !node.isObject() || !node.isMember(member)) {
            CMFrm_Warning("Json node is null or not object or has no member %s", member.c_str());
            return false;
        }

        try {
            value = node[member].as<T>();
        } catch (...) {
            CMFrm_Error("Failed to get node value, member(%s).", member.c_str());
            return false;
        }

        return true;
    }

}  // namespace SysAgent
#endif // SYSAGENT_JSON_CODER_H