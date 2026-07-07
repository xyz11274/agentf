//
// Created by z00913804 on 25-12-8.
//
#include "ToolManager.h"
#include "Tool.h"
bool isAISFPrefix(const std::string& str) {
    if (str.length() < 4) {
        return false;
    }
    return str.substr(0, 4) == "aisf";
}

bool parseInputSchema(const std::string& inputJson, InputSchema& schema)
{
    Json::Value root;
    JSONCPP_STRING errs;
    Json::CharReaderBuilder builder;

    std::istringstream jsonStream(inputJson);

    if (!Json::parseFromStream(builder, jsonStream, &root, &errs)) {
        std::cerr << "JSON 解析失败: " << errs << std::endl;
        return false;
    }

    // 解析 type
    if (!root.isMember("type")) {
        std::cerr << "缺少 type 字段" << std::endl;
        return false;
    }
    schema.type = root["type"].asString();

    // 解析 required
    if (!root.isMember("required") || !root["required"].isArray()) {
        std::cerr << "缺少 required 数组" << std::endl;
        return false;
    }
    for (const auto& item : root["required"]) {
        schema.required.push_back(item.asString());
    }

    // 解析 properties
    if (!root.isMember("properties") || !root["properties"].isObject()) {
        std::cerr << "缺少 properties 对象" << std::endl;
        return false;
    }

    const Json::Value& properties = root["properties"];
    for (const auto& propertyName : properties.getMemberNames()) {
        const Json::Value& property = properties[propertyName];
        std::map<std::string, std::string> fieldProperties;

        for (const auto& subName : property.getMemberNames()) {
            fieldProperties[subName] = property[subName].asString();
        }

        schema.properties[propertyName] = fieldProperties;
    }

    return true;
}

void ToolManager::queryToolInfo(ThreadSafeVector<MCPTool>& toolInfos) {
    LOG_INFO_EX("开始获取工具列表");
    std::vector<mmsdk::MMListToolResult> tools;
    if (Tool::ListTools(tools)) {
        LOG_INFO_EX("获取到 [%d] 个工具", tools.size());
        toolInfos.clear();
        for (const auto& tool : tools) {
            if (!isAISFPrefix(tool.name)) {
                continue; // 去掉非aisf开头的工具
            }
            LOG_INFO_EX("tool name : %s, description : %s",
                        tool.name.c_str(), tool.description.c_str());
            // JSON格式的标准MCP：{"type":"", "properties":{}, "required":[], "description" : ""}
            InputSchema inputSchema;
            if (parseInputSchema(tool.inputSchema, inputSchema)) {
                MCPTool mcpTool;
                mcpTool.name = tool.name;
                mcpTool.description = tool.description;
                mcpTool.inputSchema = inputSchema;
                toolInfos.push_back(mcpTool);
            }
        }
    }
}
std::map<std::string, ToolMessage> ToolManager::ConvertMCPToolsToToolMessages(
        const std::vector<MCPTool>& tools) {
    std::map<std::string, ToolMessage> result;

    for (const auto& tool : tools) {
        ToolMessage msg;
        msg.type = "MCP";
        msg.name = tool.name;
        msg.id = tool.name;
        msg.description = tool.description;
        msg.msgCallBack = ""; // 可根据业务设置

        ToolInterface interfaceItem;
        interfaceItem.apiName = "Install"; // 可根据 name 动态生成
        interfaceItem.apiDescription = "安装"; // 可根据 name 动态生成

        for (const auto& [key, value] : tool.inputSchema.properties) {
            ToolParam param;
            auto it = value.find("type");
            if (it != value.end()) {
                param.type = it->second == "string" ? "String" : "Int"; // number, string
            }
            it = value.find("description");
            if (it != value.end()) {
                param.description = it->second;
            }

            bool isRequired = false;
            for (const auto& req : tool.inputSchema.required) {
                if (req == key) {
                    isRequired = true;
                    break;
                }
            }
            param.required = isRequired ? "Y" : "N";
            param.source = "";
            param.needModelExtract = false;

            if (key == "user_token") { // 对于不需要提参的
                param.source = "Payload.Params.Token";
                param.description += " (该参数不需要提取)";
            }
            if (key == "device_id") {
                param.source = "Header.DeviceId";
                param.description += " (该参数不需要提取)";
            }
            interfaceItem.apiParams[key] = param;
        }

        msg.toolInterfaces.push_back(interfaceItem);
        result.insert(std::make_pair(msg.id, msg));
    }
    return result;
}