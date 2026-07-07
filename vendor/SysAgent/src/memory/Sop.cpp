//
// Created by c00888021 on 2025/2/19.
//

#include "memory/Sop.h"
#include "aisf_platform/common_define.h"
#include "aisf_platform/utils/utils.h"
#include "json/json.h"
#include "memory/GaussClient.h"
#include "rag/manager/rag_manager.h"
#include "rag/retriever/table_option.h"
#include "rag/retriever/table_value.h"
#include <fstream>
#include <memory>
#include <vector>

using namespace DM::RAG;

float THRESHOLD = 4.5;

bool SOPSet::LoadLocalSOP()
{
    std::string fileName = "/opt/coremind/conf/app_plan.json";

    std::ifstream file(fileName);
    if (!file.is_open()) {
        LOG_ERR_EX("Failed to open file: %s", fileName.c_str());
    }

    std::string line;
    while (std::getline(file, line)) {
        // 创建JSON读对象
        Json::CharReaderBuilder readerBuilder;
        Json::Value jsonData;
        std::string errs;

        // 解析JSON
        std::istringstream iss(line);
        if (Json::parseFromStream(readerBuilder, iss, &jsonData, &errs)) {
            // JSON转为sop对象
            SOPSet sopSet;
            sopSet.FromJson(jsonData);
            sopSet.Insert();
        } else {
            LOG_ERR_EX("failed to parse json: %s", errs.c_str());
        }
    }
    file.close();
    return true;
}

void SOPSet::Insert()
{
    GaussClient* client = GaussClient::Instance();
    for (auto instruction : this->instructions) {
        const map<string, string> embeddingMap{{"instruction", "instruction_vector"}};
        client->Insert("SOP", {"date", "id", "task", "description", "apps", "content", "instruction"},
            {std::make_shared<TextValue>(AisfPlat::Utils::GetCurrentDate()), std::make_shared<TextValue>(AisfPlat::Utils::GenerateUniqueId()),
                std::make_shared<TextValue>(this->task), std::make_shared<TextValue>(this->description),
                std::make_shared<TextValue>(AisfPlat::Utils::VectorToJsonString(this->apps)), std::make_shared<TextValue>(this->content), std::make_shared<TextValue>(instruction)},
            InsertOption::Builder().WithSchemaName(SCEMA_NAME).WithEmbeddingMap(embeddingMap).Build());
    }
}

void SOPSet::FromJson(const Json::Value& jsonData)
{
    this->task = jsonData.get("task", "").asString();
    this->description = jsonData.get("description", "").asString();
    this->content = jsonData.get("content", "").asString();
    std::vector<std::string> apps;
    std::vector<std::string> instructions;
    if (jsonData.isMember("apps") && jsonData["apps"].isArray()) {
        for (auto app : jsonData["apps"]) {
            apps.push_back(app.asString());
        }
    }
    if (jsonData.isMember("instructions") && jsonData["instructions"].isArray()) {
        for (auto app : jsonData["instructions"]) {
            instructions.push_back(app.asString());
        }
    }
    this->apps = apps;
    this->instructions = instructions;
}

std::vector<std::string> SOP::SimilaritySearch(std::string instruction, std::vector<std::string> fields)
{
    // 找出最相似的一个元素，和阈值进行比较
    std::vector<std::vector<std::string>> result;
    GaussClient* client = GaussClient::Instance();
    client->Search("SOP",
        QueryOption::Builder(TableQueryType::VECTOR)
            .WithBaseSchemaName(SCEMA_NAME)
            .WithVectorDistanceAlgorithm(VectorDistanceAlgorithm::L2)
            .WithVectorField("instruction_vector")
            .WithVectorTopK(1)
            .WithBaseOrder(QueryOption::QueryOrder::ASC)
            .WithVectorQuery(instruction)
            .WithBaseSelectFields(fields)
            .WithVectorReturnDistance()
            .Build(),
        result);
    if (result.size() > 0 && stof(result[0][0]) < THRESHOLD) {
        return result[0];
    }
    return std::vector<std::string>();
}

std::vector<std::string> SOP::SimilaritySearchTest(std::string instruction, std::vector<std::string> fields)
{
    // 找出最相似的一个元素，和阈值进行比较
    std::vector<std::vector<std::string>> result;
    GaussClient* client = GaussClient::Instance();
    LOG_INFO_EX("start query:%s", instruction.c_str());
    client->Search("SOP",
        QueryOption::Builder(TableQueryType::VECTOR)
            .WithBaseSchemaName(SCEMA_NAME)
            .WithVectorDistanceAlgorithm(VectorDistanceAlgorithm::L2)
            .WithVectorField("instruction_vector")
            .WithVectorTopK(1)
            .WithBaseOrder(QueryOption::QueryOrder::ASC)
            .WithVectorQuery(instruction)
            .WithBaseSelectFields(fields)
            .WithVectorReturnDistance()
            .Build(),
        result);
    LOG_INFO_EX("Test connection");
    return std::vector<std::string>();
}
