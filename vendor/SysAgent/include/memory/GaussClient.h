//
// Created by c00888021 on 2025/2/24.
//

#ifndef SYSAGENT_GAUSSCLIENT_H
#define SYSAGENT_GAUSSCLIENT_H

#include <vector>
#include <shared_mutex>
#include <string>

#include "rag/retriever/base_retriever.h"
#include "rag/retriever/table_option.h"

const std::string SCEMA_NAME = "aisf_systemagent";
const std::string AISF_LOCATION_SCEMA_NAME = "aisf_systemagent";
using InvokeCallback = std::function<void(DM::RAG::DBOperateCode retCode,
        const std::vector<std::vector<std::string>> &result)>;

class GaussClient {
public:
    static GaussClient* Instance();

    bool CreateClientAndSchema();

    bool TableExist(const std::string& tableName, const std::string& schemaName, bool& exist);

    bool CreateTable(const std::string& tableName, std::shared_ptr<DM::RAG::CreateTableOption> option);

    bool DropTable(const std::string& tableName, std::shared_ptr<DM::RAG::DropTableOption> option);

    bool CreateIndex(const std::string& tableName, std::shared_ptr<DM::RAG::CreateIndexOption> option);

    bool Insert(const std::string& tableName, const std::vector<std::string>& fields, const std::vector<std::shared_ptr<DM::RAG::TableValue>>& values,
        std::shared_ptr<DM::RAG::InsertOption> option);

    bool BatchInsert(const std::string& tableName, const std::vector<std::string>& fields, const std::vector<std::vector<std::shared_ptr<DM::RAG::TableValue>>>& values,
        std::shared_ptr<DM::RAG::InsertOption> option);

    bool Search(const std::string& tableName, std::shared_ptr<DM::RAG::QueryOption> option, std::vector<std::vector<std::string>>& result);

    bool Update(const std::string& tableName, const std::map<std::string, std::shared_ptr<DM::RAG::TableValue>>& updates, std::shared_ptr<DM::RAG::UpdateOption> option);

    bool ScalarSearch(const std::string& tableName, std::shared_ptr<DM::RAG::QueryOption> option, std::vector<std::vector<std::string>>& result);

    bool Delete(const std::string& tableName, std::shared_ptr<DM::RAG::DeleteOption> option);

    void SearchAsync(const std::string& tableName,
                            std::shared_ptr<DM::RAG::QueryOption> option,
                            const InvokeCallback& callback);

private:
    static GaussClient* instance;
    static std::shared_ptr<DM::RAG::BaseRetriever> client;

    GaussClient() {};

    ~GaussClient() {};

    GaussClient(const GaussClient&) = delete;

    GaussClient& operator=(const GaussClient&) = delete;
};

#endif // SYSAGENT_GAUSSCLIENT_H
