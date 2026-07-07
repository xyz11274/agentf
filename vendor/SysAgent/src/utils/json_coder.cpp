#include "framework_common/utils/file_util.h"

#include "utils/json_coder.h"

using namespace SYSAGENT;
bool JsonCoder::Parse(const char *data, Json::Value &root)
{
    if  (data == nullptr) {
        CMFrm_Error("Json data is null");
        return false;
    }

    int len = strlen(data);
    if (len > JSON_MAX_LEN) {
        CMFrm_Error("Json data is too long, len=%d", len);
        return false;
    }

    Json::CharReaderBuilder builder;
    std::string err;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(data, data + len, &root, &err)) {
        CMFrm_Error("json parse failed, err： %s", err.c_str());
        return false;
    }

    return !root.isNull();
}

bool JsonCoder::ParseFromFile(const std::string& filePath, Json::Value &root)
{
    std::string data;

    if (!CMFrm::Utils::ReadFile(filePath, data)) {
        CMFrm_Error("Read file failed for json parse.");
        return false;
    }

    return Parse(data.c_str(), root);
}

bool JsonCoder::IsMember(const Json::Value& node, const std::string& key)
{
    try {
        return node.isMember(key);
    } catch (...) {
        return false;
    }
}

const Json::Value JsonCoder::GetNode(const Json::Value &node, const std::string &member)
{
    if (node.isNull() || !node.isObject() || !node.isMember(member)) {
        CMFrm_Warning("Json node is null or not object or has no member %s", member.c_str());
        return Json::Value();
    }

    return node[member];
}

std::string JsonCoder::Write(const Json::Value &root)
{
    Json::StreamWriterBuilder builder;
    builder.settings_["emitUTF8"] = true;
    return Json::writeString(builder, root);
}

std::string JsonCoder::WriteWithIndentation(const Json::Value &root)
{
    Json::StreamWriterBuilder builder;
    builder.settings_["emitUTF8"] = true;
    builder.settings_["indentation"] = "";
    return Json::writeString(builder, root);
}