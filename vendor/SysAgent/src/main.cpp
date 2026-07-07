/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: 测试QATask消息体构建和sendRestfulRequest功能的简化主函数
 */

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <filesystem>

#include "framework_common/utils/file_util.h"
#include "framework_common/timermanager/timer_manager.h"
#include "framework_starter/framework_starter.h"
#include "framework_common/utils/retry_util.h"
#include "framework_common/utils/shell_util.h"

#include "AgentManager.h"
#include "microservice/microservice_model.h"
#include "microservice/microservice_manager.h"
#include "graphManager/GraphManager.h"
#include "http_client_factory.h"
#include "TeHelper.h"
#include "aisf_platform/parser/aisf_msg_parse.h"
#include "callBack/callBack.h"
#include "config/Config.h"
#include "memory/GaussClient.h"
#include "memory/SafetyFence.h"
#include "memory/Sop.h"
#include "mm_prompttmplt_manager.h"
#include "mm_sdk.h"
#include "model/PromptManager.h"
#include "msgApi.h"
#include "queue/QueueManager.h"
#include "server/Server.h"
#include "ruleCognition/RuleHandlerManager.h"
#include "queue/Lifecycle/LifecycleHandler.h"
#include "utils/env_util.h"
#include "utils/http_client.h"
#include "message/ServerEntity.h"
#include "registerManage/AgentManager.h"
#include "registerManage/ToolManager.h"
#include "sys_agent/AgentFlowManager.h"

using namespace SYSAGENT;
using namespace DM::RAG;

#define CHECK_TASKFLOW_TIMEOUT_INTERVAL 5000

// WebSocket服务器实例 (新流程不使用，关闭)
// WebSocketServer wss("websock", 8, 8000, CallBackCtr::WsServerCallback);

// Location table to save latitude and longitude, created in the schema "aisf_location"
bool InitLocationDatabase()
{
    bool result = CMFrm::Utils::Retry(
        []() -> bool {
            LOG_INFO("InitLocationDatabase, initializing Location database.");
            
            GaussClient* client = GaussClient::Instance();
            if (client == nullptr) {
                LOG_INFO_EX("Get client instance failed");
                return false;
            }
            bool exist = false;
            if (!client->TableExist("Location", AISF_LOCATION_SCEMA_NAME, exist)) {
                LOG_INFO_EX("check Location table exist failed");
                return false;
            }
            if (exist) {
                return true;
            }
            
            shared_ptr<DropTableOption> dropTableOption =
                DropTableOption::Builder().WithSchemaName(AISF_LOCATION_SCEMA_NAME).Build();
            client->DropTable("Location", dropTableOption);
            shared_ptr<CreateTableOption> option =
                CreateTableOption::Builder()
                    .WithSchemaName(AISF_LOCATION_SCEMA_NAME)
                    .WithFields({
                        TableFieldProperty::Builder("id", TableFieldType::VARCHAR, 32)
                            .WithExProperty(ExFieldProperty::Builder().WithPrimaryKey().Build())
                            .Build(),
                        TableFieldProperty::Builder("deviceID", TableFieldType::VARCHAR, 1280)
                            .WithExProperty(ExFieldProperty::Builder().WithUnique().Build())
                            .Build(),
                        TableFieldProperty::Builder("latitude", TableFieldType::VARCHAR, 1280).Build(),
                        TableFieldProperty::Builder("longitude", TableFieldType::VARCHAR, 1280).Build(),
                        TableFieldProperty::Builder("city", TableFieldType::VARCHAR, 1280).Build(),
                        TableFieldProperty::Builder("address", TableFieldType::VARCHAR, 2560).Build(),
                        TableFieldProperty::Builder("instanceNum", TableFieldType::VARCHAR, 1280).Build(),
                        TableFieldProperty::Builder("district", TableFieldType::VARCHAR, 1280).Build(),
                        TableFieldProperty::Builder("town", TableFieldType::VARCHAR, 1280).Build(),
                    })
                    .WithReportIfExists(true)
                    .Build();
            
            bool createResult = client->CreateTable("Location", option);
            CHECK_TRUE_LOG_ERROR_RETURN(!createResult, false, "Create Location table failed");

            LOG_INFO_EX("Location database initialized successfully");
            return true;
        },
        CMFrm::Constants::RetryCount2, CMFrm::Constants::RetryBackOffSecond2);
    if (!result) {
        LOG_ERR_EX("Failed to initialize Location database");
        return false;
    }

    return true;
}

bool InitSafetyFenceDatabase()
{
    GaussClient* client = GaussClient::Instance();
    if (!client) {
        LOG_INFO_EX("Get client instance failed");
        return false;
    }
    bool exist = false;
    if (!client->TableExist("safetyfence", AISF_LOCATION_SCEMA_NAME, exist)) {
        LOG_INFO_EX("check table exist failed");
        return false;
    }

    // 如果表存在，则进行删除
    if (exist) {
        shared_ptr<DropTableOption> dropTableOption =
                DropTableOption::Builder().WithSchemaName(AISF_LOCATION_SCEMA_NAME).Build();
        client->DropTable("safetyfence", dropTableOption);
    }

    shared_ptr<CreateTableOption> option =
        CreateTableOption::Builder()
            .WithSchemaName(AISF_LOCATION_SCEMA_NAME)
            .WithFields({
                TableFieldProperty::Builder("id", TableFieldType::VARCHAR, 32)
                    .WithExProperty(ExFieldProperty::Builder().WithPrimaryKey().Build())
                    .Build(),
                TableFieldProperty::Builder("query", TableFieldType::VARCHAR, 1280)
                    .WithExProperty(ExFieldProperty::Builder().WithUnique().Build())
                    .Build(),
                TableFieldProperty::Builder("query_vector", TableFieldType::FLOATVECTOR, 512).Build(),
                TableFieldProperty::Builder("content", TableFieldType::VARCHAR, 1280).Build(),
                TableFieldProperty::Builder("scene_type", TableFieldType::VARCHAR, 32).Build(),
            })
            .WithReportIfExists(true)
            .Build();
    return client->CreateTable("safetyfence", option);
}

bool InitSOPDatabase()
{
    GaussClient* client = GaussClient::Instance();
    if (client != nullptr) {
        LOG_INFO_EX("Get client instance success");
    }
    shared_ptr<DropTableOption> dropTableOption = DropTableOption::Builder().WithSchemaName(SCEMA_NAME).Build();
    client->DropTable("SOP", dropTableOption);
    const map<string, string> embeddingMap{{"instruction", "instruction_vector"}};
    shared_ptr<CreateTableOption> option =
        CreateTableOption::Builder()
            .WithSchemaName(SCEMA_NAME)
            .WithFields({
                TableFieldProperty::Builder("id", TableFieldType::VARCHAR, 32)
                    .WithExProperty(ExFieldProperty::Builder().WithPrimaryKey().Build())
                    .Build(),
                TableFieldProperty::Builder("date", TableFieldType::VARCHAR, 1280).Build(),
                TableFieldProperty::Builder("task", TableFieldType::VARCHAR, 1280).Build(),
                TableFieldProperty::Builder("description", TableFieldType::VARCHAR, 2560).Build(),
                TableFieldProperty::Builder("apps", TableFieldType::VARCHAR, 2560).Build(),
                TableFieldProperty::Builder("content", TableFieldType::VARCHAR, 2560).Build(),
                TableFieldProperty::Builder("instruction", TableFieldType::VARCHAR, 2560).Build(),
                TableFieldProperty::Builder("instruction_vector", TableFieldType::FLOATVECTOR, 512).Build(),
            })
            .WithEmbeddingMap(embeddingMap)
            .WithReportIfExists(true)
            .Build();
    return client->CreateTable("SOP", option);
}

void AgentFlowInit(const std::string& agentName)
{
    SysAgent::AgentInitConfig agentInitConfig;
    std::string agentBasePath = "/opt/coremind/agents/";
    agentInitConfig.agentName = agentName;
    agentInitConfig.messageCallback = SysCallBack::SysAgentMsgCallBack;
    agentInitConfig.agentPackagePath = agentBasePath + agentName;
    std::filesystem::current_path(agentBasePath + agentName);

    SysAgent::AgentFlowInitConfig agentFlowInitConfig;
    agentFlowInitConfig.agents.push_back(agentInitConfig);
    SysAgent::AgentFlowManager::GetAgentFlowManager()->Init(agentFlowInitConfig);
}

uint64_t SysAgentInit()
{
    if (!ConfigManager::getInstance().LoadConfig()) {
        std::cout << "load config failed" << std::endl;
        return 1;
    }
    PromptManager::GetInstance().Init();
    auto result = InitLocationDatabase();
    if (!result) {
        LOG_ERR_EX("============ Init Location Database Failed! ============");
        return 1;
    } else {
        LOG_INFO_EX("============ Init Location Database Success! ============");
    }

    auto ret = InitSafetyFenceDatabase();
    if (!ret) {
        LOG_ERR_EX("============ Init SafetyFence Database Failed! ============");
        return 1;
    } else {
        LoadLocalSafetyFence();
        LOG_INFO_EX("============ Init SafetyFence Database Success! ============");
    }

    return 0;
}

bool InitUMOMFrm()
{
    auto HttpClientFactory = CMFrm::COM::HttpClientFactory::GetHttpClientFactory();
    auto modelConfig = std::make_shared<CMFrm::COM::Http1ClientConfig>();
    modelConfig->name = "http1_default";
    HttpClientFactory->RegisterHttp1ClientConfig(modelConfig);

    auto movieConfig = std::make_shared<CMFrm::COM::Http1ClientConfig>();
    movieConfig->name = "MovieAgent";
    HttpClientFactory->RegisterHttp1ClientConfig(movieConfig);

    auto kmmConfig = std::make_shared<CMFrm::COM::Http2ClientConfig>();
    kmmConfig->name = "KMMService";
    kmmConfig->connectionPoolSize = 2;
    kmmConfig->workerPoolSize = 4;
    HttpClientFactory->RegisterHttp2ClientConfig(kmmConfig);

    auto streamConfig = std::make_shared<CMFrm::COM::Http2ClientConfig>();
    streamConfig->name = "stream";
    streamConfig->timeoutSecond = 6000;
    HttpClientFactory->RegisterHttp2ClientConfig(streamConfig);

    // MultiDevAgent、LTServerAgent、WisdomAgent
    std::vector<std::string> services = {"MovieAgent", "HealthAgent", "GUIAgent", "LifeAssistantAgent",
                                         "AnyShoppingAgent", "FoodieAgent", "KMMService",
                                         "CustomAgent", "QAAgent", "ThirdProxyAgent",
                                         "MMGuardService", "StudyBuddyAgent", "IMInferScheduler"};
    for (auto& service : services) {
        if(!HttpClient::QueryServiceInstance(service)){
            LOG_ERR("HttpClient::QueryServiceInstance ERROR!");
            return false;
        }
    }
    return true;
}

void InitMMGateWayInstanceChange()
{
    auto appid = CMFrm::Utils::GetEnv("APPID", "0");
    CMFrm::OM::MicroserviceManager::GetMicroserviceManager()->SubscribeServiceInstanceChangeCallback(
            CMFrm::OM::ServiceDiscoveryType::ManagerServiceDiscovery,
            {CMFrm::OM::ServiceKey(appid, "MMGateWay", "")},
            [](const CMFrm::OM::MicroServiceInstanceChangedEvent& event) {
                LOG_INFO("MMGateWay instance status changed");
                CMFrm::OM::QueryInstanceExtResp resp;
                std::string url;
                if (HttpClient::GetAllInstanceInfo("MMGateWay", resp) || resp.instanceList.empty()) {
                    LOG_ERR("get mmGateWay service info failed.");
                    return;
                }

                std::string endPoint;
                for (auto &inst : resp.instanceList) {
                    if (inst.haStatus == CMFrm::OM::HAStatus::MasterHAStatus) {
                        endPoint = inst.endpoint;
                    }
                }
                LOG_INFO("mmGateWay inst ip:%s", endPoint.c_str());
                auto epList = CMFrm::Utils::SplitStr(endPoint, ":");
                if (epList.empty()) {
                    LOG_ERR("SplitStr failed, endpoint:%s.", endPoint.c_str());
                    return;
                }

                HttpClient::UpdateInstanceInfo("MMGateWay", epList[0]);
            });
}

void NotifyServiceInstanceChange()
{
    LOG_INFO_EX("enter callback SubscribeServiceInstanceChangeCallback");
    // 订阅服务事件通知
    auto appid = CMFrm::Utils::GetEnv("APPID", "0");
    auto svcManager = CMFrm::OM::MicroserviceManager::GetMicroserviceManager();
    std::vector<CMFrm::OM::ServiceKey> servicesList = {
            CMFrm::OM::ServiceKey(appid, "MovieAgent", "0+"),
            CMFrm::OM::ServiceKey(appid, "HealthAgent", "0+"),
            CMFrm::OM::ServiceKey(appid, "GUIAgent", "0+"),
            CMFrm::OM::ServiceKey(appid, "MultiDevAgent", "0+"),
            CMFrm::OM::ServiceKey(appid, "LifeAssistantAgent", "0+"),
            CMFrm::OM::ServiceKey(appid, "AnyShoppingAgent", "0+"),
            CMFrm::OM::ServiceKey(appid, "FoodieAgent", "0+"),
            CMFrm::OM::ServiceKey(appid, "LTServerAgent", "0+"),
            CMFrm::OM::ServiceKey(appid, "WisdomAgent", "0+"),
            CMFrm::OM::ServiceKey(appid, "CustomAgent", "0+"),
            CMFrm::OM::ServiceKey(appid, "KMMService", "0+"),
            CMFrm::OM::ServiceKey(appid, "QAAgent", "0+"),
            CMFrm::OM::ServiceKey(appid, "ThirdProxyAgent", "0+"),
            CMFrm::OM::ServiceKey(appid, "MMGuardService", "0+"),
            CMFrm::OM::ServiceKey(appid, "OfficeAgent", "0+"),
            CMFrm::OM::ServiceKey(appid, "StudyBuddyAgent", "0+"),
            CMFrm::OM::ServiceKey(appid, "DMContextService", "0+"),
            CMFrm::OM::ServiceKey(appid, "AISFMgr", "0+"),
            CMFrm::OM::ServiceKey(appid, "IMInferScheduler", "0+")
    };

    svcManager->SubscribeServiceInstanceChangeCallback(
        CMFrm::OM::ServiceDiscoveryType::BusinessServiceDiscovery, servicesList,
        [](const CMFrm::OM::MicroServiceInstanceChangedEvent& event) {
            LOG_INFO_EX("receive event: %s", event.JSONString().c_str());
            if (event.action == CMFrm::OM::InstanceChangeAction::CREATE_ACTION) {
                auto endpoint = event.endpoints.at("rest");
                size_t pos = endpoint.rfind(':');
                if (pos != std::string::npos) {
                    endpoint = endpoint.substr(0, pos);
                }  // 框架优先将同节点的服务放在第一个
                HttpClient::UpdateInstanceInfo(event.serviceName, endpoint);
            } else if (event.action == CMFrm::OM::InstanceChangeAction::DELETE_ACTION) {
                HttpClient::DeleteInstanceInfo(event.serviceName);
            }
        });

    InitMMGateWayInstanceChange();
    LOG_INFO_EX("SubscribeServiceInstanceChangeCallback finished");
}

bool initCoreMindFrm(int argc, char* argv[])
{
    if (!SysCallBack::RegisterRouter()) {
        LOG_STDERR("Register router failed.\n");
        return false;
    }

    CMFrm::Starter::FrameworkInitParam frameworkInitParam;
    frameworkInitParam.SetProcessArgs(argc, argv);
    frameworkInitParam.AddServiceRouter(SysCallBack::GetRouterMgr());

    CMFrm::Starter::FrameworkStarter::InitFramework(frameworkInitParam);
    LOG_INFO("CoreMindFrm init success.");
    return true;
}

bool initMateMindSdk()
{
    auto logFunc = [](mmsdk::MMlogLevel level, const std::string& str) -> uint32_t {
        if (level == mmsdk::MM_LOG_DEBUG) {
            LOG_DEBUG("%s", str.c_str());
        } else if (level == mmsdk::MM_LOG_INFO) {
            LOG_INFO("%s", str.c_str());
        } else if (level == mmsdk::MM_LOG_WARN) {
            LOG_WARN("%s", str.c_str());
        } else if (level == mmsdk::MM_LOG_ERR || level == mmsdk::MM_LOG_FATAL) {
            LOG_ERR("%s", str.c_str());
        }
        return 0;
    };
    if (mmsdk::MM_SdkInit("/opt/coremind/conf/matemindsdk.json", logFunc) != 0) {
        LOG_ERR("MateMateSdk init failed.");
        return false;
    }
    LOG_INFO("MateMind init success.");
    return true;
}

void initScheduledTasks()
{
    // 定时任务1：遍历workerflow，检查超时任务
    auto timeoutCheckTimerTask = std::make_shared<CMFrm::Timer::TimerTask>(0, CHECK_TASKFLOW_TIMEOUT_INTERVAL, [](){
        auto tes = TEManager::getInstance().getAllTEs();
        for (auto& te : tes) {
            te->stopTimeoutWorkFlow();
        }
    });
    CMFrm::Timer::TimerManager::GetTimerManager()->AddTimerTask(timeoutCheckTimerTask);

    // 定时任务2：清理ModelLog
    CMFrm::Timer::TimerManager::GetTimerManager()->AddTimerTask(std::make_shared<CMFrm::Timer::TimerTask>(
            0, 1000 * 60 * 60, []() {
                CMFrm::Utils::ExecuteShell("/opt/coremind/scripts/model_log_persistence.sh", {});
            }));
}

int main(int argc, char* argv[])
{
    LOG_STDOUT("===== SysAgent 开始初始化  =====");

    // 1. 初始化系统
    LOG_STDOUT("系统初始化开始");
    srand(time(0));

    if (!initCoreMindFrm(argc, argv)) {
        LOG_ERR("CoreMindFrm init failed.");
        return -1;
    }

    if (!initMateMindSdk()) {
        LOG_ERR("MateMind init failed.");
        return -1;
    }

    if(!InitUMOMFrm()) {
        LOG_ERR("UMOMFrm init failed.");
        return -1;
    }

    NotifyServiceInstanceChange();

    if (SysAgentInit() != 0) { // 完成各个类的初始化，需要将所有的url注册后，再拉起RestServer
        LOG_ERR("==== SysAgentInit ERROR!========");
        return -1;
    }

    if(!HttpClient::RegisterHttp1ClientConfig()){
        LOG_ERR("RegisterHttp1ClientConfig ERROR!");
        return -1;
    }

    // 初始化Agent (新流程不使用，关闭 SendAsync 源)
    // AgentManager::getInstance().init();
    // std::cout << "===== Agents initialized =====" << std::endl;
    // 起定时任务获取全量工具 (内部 SendAsync，关闭)
    // ToolManager::getInstance().init();
    // std::cout << "===== ToolManager initialized =====" << std::endl;
    // 2. 初始化REST服务和WebSocket
    // WebSocket / ReadQueue / WriteQueue 新流程不使用，关闭避免旧路径线程阻塞
    /*
    QueueManager::getInstance().init();
    LOG_STDOUT("=====Queue initialized=====");

    RuleHandlerManager::getInstance().init();
    LOG_STDOUT("=====RuleHandlerManager initialized=====");

    LifecycleHandlerManager::GetInstance().Init();
    LOG_STDOUT("=====LifecycleHandlerManager initialized=====");

    GraphManager::GetInstance().Init();

    if (!wss.Start()){
        LOG_ERR("WebSocketServer start failed.");
        return -1;
    }
    LOG_STDOUT("=====SystemAgent initialized=====");
    LOG_INFO("REST service and WebSocket initialized");
    */

    // 初始化定时任务 (WebSocket + TE 已关闭，不再需要)
    // initScheduledTasks();

    const char* envAgentVersion = std::getenv("SYSTEM_AGENT_NAME");
    AgentFlowInit(envAgentVersion);

    // ── GUI 桥接超时检查线程 ──
    extern void StartGuiTimeoutThread();
    StartGuiTimeoutThread();

    CMFrm::Starter::FrameworkStarter::WaitProcessExit();

    // 释放资源并停止服务
    // wss.Stop();  // WebSocket 已关闭
    LOG_INFO("end of test，WebSocket service terminated");
    return 0;
}