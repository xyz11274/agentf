//
// Created by c00888021 on 2025/2/19.
//

#include "memory/SafetyFence.h"
#include "aisf_platform/common_define.h"
#include "aisf_platform/utils/utils.h"
#include "json/json.h"
#include "memory/GaussClient.h"
#include "rag/manager/rag_manager.h"
#include "rag/retriever/table_option.h"
#include "rag/retriever/table_value.h"
#include <fstream>
#include <functional>
#include <memory>
#include <vector>
#include "aisf_platform/parser/json_parser.h"
#include "utils/env_util.h"
#include "config/Config.h"

using namespace DM::RAG;

namespace SYSAGENT {

float THRESHOLD = 0.3;

bool LoadLocalSafetyFence()
{
    std::vector<std::string> fileList = {"/opt/coremind/conf/safetyfence.json",
                                         "/opt/coremind/conf/video_command.json"};
    for (auto fileName : fileList) {
        LOG_INFO_EX("start open file:%s", fileName.c_str());
        // 打开JSON文件
        char* canonicalPath = realpath(fileName.c_str(), nullptr);
        if (canonicalPath == nullptr) {
            LOG_ERR_EX("realpath %s failed", fileName.c_str());
        }
        std::ifstream file(canonicalPath, std::ifstream::binary);
        if (!file.is_open()) {
            LOG_ERR_EX("open %s failed", fileName.c_str());
            free(canonicalPath);
            return false;
        }

        // 读取JSON文件到Json::Value
        Json::Value jsonData;
        Json::CharReaderBuilder rbuilder;
        std::string errs;
        if (!Json::parseFromStream(rbuilder, file, &jsonData, &errs)) {
            LOG_ERR_EX("Failed to parse safetyfence.json");
            free(canonicalPath);
            return false;
        }

        if (!jsonData.isArray()) {
            LOG_ERR_EX("Failed to parse safetyfence.json");
            free(canonicalPath);
            return false;
        }
        std::vector<SafetyFence> sfs;

        for (const auto& item : jsonData) {
            SafetyFence sf(item["query"].asString(), item["content"].asString(), item["scene_type"].asString());
            sfs.push_back(sf);
        }

        Insert(sfs);
        free(canonicalPath);
    }

    return true;
}

#define batch_size 1
void Insert(const std::vector<SafetyFence>& sfs)
{
    GaussClient* client = GaussClient::Instance();
    const map<string, string> embeddingMap{{"query", "query_vector"}};
    auto process_batch_iter = [&embeddingMap, client](std::vector<SafetyFence>::const_iterator begin, std::vector<SafetyFence>::const_iterator end) -> bool {
        auto insert = std::vector<std::vector<std::shared_ptr<TableValue>>>();
        for (auto it = begin; it != end; ++it) {
            insert.push_back({
                std::make_shared<TextValue>(AisfPlat::Utils::GenerateUniqueId()),
                std::make_shared<TextValue>(it->query),
                std::make_shared<TextValue>(it->content),
                std::make_shared<TextValue>(it->sceneType)
            });
        }
        return client->BatchInsert("SafetyFence", {"id", "query", "content", "scene_type"}, insert, InsertOption::Builder().WithSchemaName(SCEMA_NAME).WithEmbeddingMap(embeddingMap).Build());
    };
    for (size_t i = 0; i < sfs.size(); i += batch_size) {
        size_t end = std::min(i + batch_size, sfs.size());
        bool result = process_batch_iter(sfs.begin() + i, sfs.begin() + end);
        if (!result) {
            LOG_ERR_EX("Failed to batch insert data: %s", sfs[i].query.c_str());
        }
    }
    auto opt = CreateIndexOption::Builder({"query_vector"}, "query_vector", CreateIndexOption::TableIndexType::LITE_VECTOR)
        .WithSchemaName(AISF_LOCATION_SCEMA_NAME)
        .WithVectorIndexOption(VectorIndexOption::Builder(VectorDistanceAlgorithm::L2, VectorIndexAlgorithm::IVFFLAT).Build())
        .Build();
    client->CreateIndex("SafetyFence", opt);
}

std::vector<std::string> SafetyFence::SimilaritySearch(std::string query, std::vector<std::string> fields)
{
    // 找出最相似的一个元素，和阈值进行比较
    std::vector<std::vector<std::string>> result;
    GaussClient* client = GaussClient::Instance();
    client->Search("SafetyFence",
        QueryOption::Builder(TableQueryType::VECTOR)
            .WithBaseSchemaName(SCEMA_NAME)
            .WithVectorDistanceAlgorithm(VectorDistanceAlgorithm::L2)
            .WithVectorField("query_vector")
            .WithVectorTopK(1)
            .WithBaseOrder(QueryOption::QueryOrder::ASC)
            .WithVectorQuery(query)
            .WithBaseSelectFields(fields)
            .WithBaseFilters(FilterOption::Builder()
            .WithFieldName("scene_type")
            .WithEQ()
            .WithValue(make_shared<VarCharValue>("1"))
            .Build())
            .WithVectorReturnDistance()
            .Build(),
        result);
    std::string thresholdStr = ConfigManager::getInstance().getEnvInfo("RAG_SAFETYFENCE_THRESHOLD", "0");
    float threshold = std::stof(thresholdStr);
    if (!result.empty()) {
        for (auto& item : result[0]) {
            LOG_INFO_EX("safetyfence rag res:%s", item.c_str());
        }
    } else {
        LOG_INFO_EX("safetyfence rag is empty");
    }
    if (result.size() > 0 && stof(result[0][0]) <= threshold) {
        return result[0];
    }
    return {};
}

std::string SafetyFence::parseRejectContent(const std::string& data)
{
    Json::Value root;
    AisfPlat::JsonParser::Parse(data, root);
    if (root.isMember("data") && root["data"].isObject()) {
        auto dataObj = root["data"];
        if (dataObj.isMember("content") && dataObj["content"].isString()) {
            return dataObj["content"].asString();
        }
    }
    return "";
}

std::string SafetyFence::parseIntentName(const std::string& data)
{
    Json::Value root;
    AisfPlat::JsonParser::Parse(data, root);
    if (root.isMember("intentName") && root["intentName"].isString()) {
        return root["intentName"].asString();
    }
    return "";
}
} // namespace SYSAGENT