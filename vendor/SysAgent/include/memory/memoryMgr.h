#ifndef SYSAGENT_MEMORY_MGR_H
#define SYSAGENT_MEMORY_MGR_H

#include <unordered_map>

#include "aisf_platform/utils/utils.h"
#include "service_router/router_manager.h"
#include "threadpool/threadpool.h"

#include "http_client_factory.h"

struct MemoryReqParam {
    std::string userId;
    std::string sceneType;
    std::string body;
    std::string query;
    std::string category;
    int topK;
    int needMemList;
};

struct QAObject {
    Json::Value originalQA;
    Json::Value abstractQA;
    string id;
    std::string abstract;
    std::string rewriteQ;
    std::string outType;
    std::string keys;
    std::string userId;

    Json::Value ToJson() {
        Json::Value root;
        root["abstract"] = abstract;
        root["rewriteQ"] = rewriteQ;
        root["outType"] = outType;
        root["keys"] = keys;
        if (!originalQA.isNull()) {
            root["originalQA"] = originalQA;
        }
        if (!abstractQA.isNull()) {
            root["abstractQA"] = abstractQA;
        }
        return root;
    }
};

using LongMemoriesCallback = std::function<void(bool isSuccess, const std::vector<std::string> &memories)>;
using ShortMemoriesCallback = std::function<void(bool isSuccess, const std::vector<std::map<std::string, std::string>> &memories)>;
using OperateMemoriesCallbback = std::function<void(bool isSuccess, const std::string &response)>;

struct MemoryRelativeInfo {
    std::string keyWord;
    int index;
};

class MemoryMgr {
    using RestProcFunc = std::function<void(const std::shared_ptr<CMFrm::ServiceRouter::Context>&)>;

public:
    enum class RequestType { RECALL_MEMORY, QUERY_MEMORY, QUERY_PERSONA };

    static MemoryMgr& GetInstance()
    {
        static MemoryMgr instance = MemoryMgr();
        return instance;
    }

    MemoryMgr();

    // 使用线程池分发
    void OnRecvRestRequestAsync(RequestType reqType, const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);
    static bool GetLongMemories(const std::string &userName, const std::string &query, int topK, const LongMemoriesCallback &callback);
    static bool GetShortMemories(const std::string &userName, const std::string &taskId, int count, const ShortMemoriesCallback &callback);
    static Json::Value GetRemoteMemories(const std::string& userName, const std::string& taskID, int count);
    static bool AddShortMemories(const std::string &taskIdr, const std::string &opType,
                                 std::shared_ptr<QAObject> qaObject, const OperateMemoriesCallbback& callback);
private:
    static void OnRecvMemoryRecall(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);
    static void OnRecvMemoryQuery(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);
    static void OnRecvPersonaQuery(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);
    static bool ParseMemRequestParam(const std::shared_ptr<CMFrm::ServiceRouter::Context>& context,
                                     MemoryReqParam& param);

    std::shared_ptr<CMFrm::ThreadPool::ThreadPool> m_threadPool;
    std::unordered_map<RequestType, RestProcFunc> m_procMap;
};

#endif // SYSAGENT_MEMORY_MGR_H