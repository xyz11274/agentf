//
// Created by c00888021 on 2025/2/24.
//
#include <iostream>
#include <memory>
#include <mutex>

#include "framework_common/utils/env_util.h"

#include "aisf_platform/common_define.h"
#include "aisf_platform/utils/utils.h"
#include "memory/GaussClient.h"
#include "rag/datasource/data_source.h"
#include "rag/embedding/base_embedding.h"
#include "rag/retriever/base_retriever.h"
#include "rag/retriever/table_option.h"
#include "rag/manager/rag_manager.h"
#include "config/Config.h"

using namespace DM::RAG;

#define TIMEOUT 30 // 数据库超时

GaussClient* GaussClient::instance = nullptr;
shared_ptr<BaseRetriever> GaussClient::client = nullptr;

GaussClient* GaussClient::Instance()
{
    if (instance == nullptr) {
        static mutex mtx;
        std::lock_guard<mutex> lock(mtx);
        if (instance == nullptr) {
            instance = new GaussClient();
            instance->CreateClientAndSchema();
        }
    }

    return instance;
}

bool GaussClient::CreateClientAndSchema()
{
    std::string appId = CMFrm::Utils::GetEnv("APPID", "0");
    std::string appName = CMFrm::Utils::GetEnv("APPNAME", "aisf");
    std::string embeddingURL = ConfigManager::getInstance().getEnvInfo("EmbeddingURL", "http://9.15.87.225:2025/embed");
    std::shared_ptr<DataSource> ds = DataSource::CreateDMDataSource(std::stoi(appId), appName, "ragdb");
    const string filePath = "/opt/coremind/embedding/model.mindir"; //"./model.onnx";
    const string vocabPath = "/opt/coremind/embedding/vocab.txt";
    const string configPath = "/opt/coremind/conf/product.json";
    const shared_ptr<BaseEmbedding> embedding = BaseEmbedding::CreateEmbedding(
        nullptr, nullptr, InferenceOption::Builder(
            DM::RAG::InferenceOption::XpuType::NPU,
            DM::RAG::InferenceOption::BackEndType::MINDSPORECLOUD).Build(),
        EmbeddingPattern::REMOTE);
    LOG_STDOUT("get embedding success");
    RagLogInfo loginfo = {.levelDebug = 0, .levelInfo = 1, .levelError = 2, .logFunc = [](uint32_t loggerLevel, const std::string& logStr) -> uint32_t {
                              if (loggerLevel == 1) {
                                  LOG_INFO("%s", logStr.c_str());
                              } else if (loggerLevel == 2) {
                                  LOG_ERR("%s", logStr.c_str());
                              } else if (loggerLevel == 0) {
                                  LOG_DEBUG("%s", logStr.c_str());
                              }
                              return 0;
                          }};
    RagInitParam param = RagInitParam(loginfo, configPath);

    RagManager::Init(param);

    client = BaseRetriever::CreateRetriever(ds, embedding);

    client->CreateSchema(SCEMA_NAME, CreateSchemaOption::Builder().WithTimeOut(TIMEOUT).Build());
    client->CreateSchema(AISF_LOCATION_SCEMA_NAME, CreateSchemaOption::Builder().WithTimeOut(TIMEOUT).Build());

    return true;
}

bool GaussClient::TableExist(const std::string& tableName, const std::string& schemaName, bool& exist)
{
    return (client->IsTableExist(tableName, exist, schemaName) == DBOperateCode::SUCCESS);
}

bool GaussClient::CreateTable(const string& tableName, shared_ptr<CreateTableOption> option)
{
    return (client->CreateTable(tableName, option) == DBOperateCode::SUCCESS);
}

bool GaussClient::DropTable(const string& tableName, shared_ptr<DropTableOption> option)
{
    return (client->DropTable(tableName, option) == DBOperateCode::SUCCESS);
}

bool GaussClient::CreateIndex(const string& tableName, shared_ptr<CreateIndexOption> option)
{
    return (client->CreateIndex(tableName, option) == DBOperateCode::SUCCESS);
}

bool GaussClient::Insert(const std::string& tableName, const std::vector<std::string>& fields, const std::vector<std::shared_ptr<DM::RAG::TableValue>>& values,
    std::shared_ptr<DM::RAG::InsertOption> option)
{
    return (client->Insert(tableName, fields, values, option) == DBOperateCode::SUCCESS);
}

bool GaussClient::BatchInsert(const std::string& tableName, const std::vector<std::string>& fields, const std::vector<std::vector<std::shared_ptr<TableValue>>>& values,
    std::shared_ptr<DM::RAG::InsertOption> option)
{
    return (client->BatchInsert(tableName, fields, values, option) == DBOperateCode::SUCCESS);
}

bool GaussClient::Search(const string& tableName, shared_ptr<QueryOption> option, vector<vector<string>>& result)
{
    return (client->VectorSearch(tableName, option, result) == DBOperateCode::SUCCESS);
}

bool GaussClient::Update(const std::string& tableName, const std::map<std::string, std::shared_ptr<TableValue>>& updates, std::shared_ptr<UpdateOption> option)
{
    return (client->Update(tableName, updates, option) == DBOperateCode::SUCCESS);
}

bool GaussClient::ScalarSearch(const std::string& tableName, std::shared_ptr<QueryOption> option, std::vector<std::vector<std::string>>& result)
{
    return (client->ScalarSearch(tableName, option, result) == DBOperateCode::SUCCESS);
}

bool GaussClient::Delete(const std::string& tableName, std::shared_ptr<DM::RAG::DeleteOption> option)
{
    return (client->Delete(tableName, option) == DBOperateCode::SUCCESS);
}

void GaussClient::SearchAsync(const std::string& tableName,
                              std::shared_ptr<DM::RAG::QueryOption> option,
                              const InvokeCallback& callback)
{
    client->VectorSearchAsync(tableName, option,
        [callback](DBOperateCode retCode,
            const std::vector<std::vector<std::string>> &result) -> DBOperateCode {
        if (retCode == DBOperateCode::SUCCESS) {
            LOG_INFO_EX("SearchAsync success");
        } else {
            LOG_INFO_EX("SearchAsync failed");
        }
        for (auto& row : result) {
            for (auto& col : row) {
                LOG_INFO_EX("rag data:%s", col.c_str());
            }
        }
        callback(retCode, result);
        return retCode;
    });
}