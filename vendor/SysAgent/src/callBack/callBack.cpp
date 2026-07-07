/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * Description: HealthAgent 垂域回调 — 消息输入输出路由（精简版）
 *
 * 相比 ATA 任务管理版本，本版本去掉了 WorkFlow / ATATask / TE 等复杂任务管理层。
 * 核心逻辑：收到消息 → 直接 SendMsg 进 AgentFlow 框架 / 转发到外部 SysAgent。
 */

#include "callBack/callBack.h"
#include "aisf_platform/common_define.h"
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <chrono>
#include <sys/stat.h>
#include "framework_common/constants/number_constant.h"
#include "utils/json_coder.h"
#include "sys_agent/AgentFlowManager.h"
#include "aisf_platform/utils/utils.h"
#include "utils/Constants.h"
#include "utils/http_client.h"
using namespace AisfPlat;

std::shared_ptr<SYSAGENT::TaskRouter> SysCallBack::routerMgr;

// ── GUI 桥接超时跟踪 ────────────────────────────────────────────────
// 在 HandleGuiBridgeForward 转发时记录，在 GUI 回调时清理，
// 超时线程定时扫描，超时后构造失败报文发送给 coordinator。

struct GuiBridgeRecord {
    std::string parentTaskID;
    int64_t     sendTimeMs;
};

static std::map<std::string, GuiBridgeRecord> g_guiBridgeMap;   // parentTaskID → record
static std::set<std::string>                  g_guiTimedOutSet;  // 已超时 parentTaskID
static std::mutex                             g_guiBridgeMutex;

static int64_t GuiTimeoutMs() {
    const char* val = std::getenv("AF_GUI_TIMEOUT_MS");
    return val ? std::stoll(val) : 6 * 60 * 1000;  // 默认 6 分钟
}

static int64_t GuiTimeoutNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void RecordGuiBridge(const std::string& parentTaskID) {
    GuiBridgeRecord rec{parentTaskID, GuiTimeoutNowMs()};
    std::lock_guard<std::mutex> lock(g_guiBridgeMutex);
    g_guiBridgeMap[parentTaskID] = rec;
    LOG_INFO_EX("[GUI_BRIDGE] RECORD: parentTaskID=%s", parentTaskID.c_str());
}

static void ClearGuiBridge(const std::string& taskID) {
    // 按 taskID 向上查找并清理（与 findHaContext 匹配逻辑一致）
    std::lock_guard<std::mutex> lock(g_guiBridgeMutex);
    std::string key = taskID;
    while (!key.empty()) {
        if (g_guiBridgeMap.erase(key)) {
            LOG_INFO_EX("[GUI_BRIDGE] CLEAR: taskID=%s -> key=%s", taskID.c_str(), key.c_str());
            return;
        }
        size_t pos = key.rfind('_');
        if (pos == std::string::npos) break;
        key = key.substr(0, pos);
    }
}

static bool IsGuiBridgeTimedOut(const std::string& taskID) {
    std::lock_guard<std::mutex> lock(g_guiBridgeMutex);
    std::string key = taskID;
    while (!key.empty()) {
        if (g_guiTimedOutSet.count(key)) return true;
        size_t pos = key.rfind('_');
        if (pos == std::string::npos) break;
        key = key.substr(0, pos);
    }
    return false;
}

// 前向声明（实现在文件末尾，可访问 findHaContext / HA_AGENT_NAME）
static void GuiTimeoutScan();
void StartGuiTimeoutThread();

// ======================== HealthAgent 垂域处理 ========================

constexpr char HA_AGENT_NAME[] = "health_agent_rust";

// ── 上游上下文存储（替代 TE extraInfo） ──
// 每次 TASKASSIGNMENT 到达时记录，供桥接转发 / 结果回传使用。
// key = parentTaskID（外部 SysAgent 下发的原始 TaskID）
// AgentFlow 内部 dialogueId/sessionId 由框架生成，首次为空；
// 桥接回调时 SysAgentMsgCallBack 带回框架内部 ID 后更新。

struct HaContext {
    std::string sysAgentEntity;
    std::string parentTaskID;
    std::string sender;
    std::string internalDialogueId;   // AgentFlow 框架内部生成的 dialogueId
    std::string internalSessionId;     // AgentFlow 框架内部生成的 sessionId
    // 用户信息（用于构建输出报文）
    std::string userName;
    std::string deviceId;
    std::string deviceCategory;
    std::string uuid;
    std::string clientSessionId;
    std::string acmSessionId;
    std::string teamEnvironmentID;
};

static std::mutex haCtxMutex;
static std::unordered_map<std::string, HaContext> haContextMap;  // key = parentTaskID

// 从 taskID 向上查找 HaContext（逐层剥离 _N 后缀，匹配 parentTaskID）
static bool findHaContext(const std::string& taskID, HaContext& ctx)
{
    std::lock_guard<std::mutex> lock(haCtxMutex);
    std::string key = taskID;
    while (!key.empty()) {
        auto it = haContextMap.find(key);
        if (it != haContextMap.end()) {
            ctx = it->second;
            return true;
        }
        // 剥掉最后一个 _N 后缀
        size_t pos = key.rfind('_');
        if (pos == std::string::npos) break;
        key = key.substr(0, pos);
    }
    return false;
}

// 写入 HaContext
static void saveHaContext(const HaContext& ctx)
{
    std::lock_guard<std::mutex> lock(haCtxMutex);
    haContextMap[ctx.parentTaskID] = ctx;
}

// 更新指定 taskID 对应 HaContext 的 AgentFlow 内部 ID
static void updateHaContextInternalIds(const std::string& taskID,
    const std::string& dialogueId, const std::string& sessionId)
{
    std::lock_guard<std::mutex> lock(haCtxMutex);
    std::string key = taskID;
    while (!key.empty()) {
        auto it = haContextMap.find(key);
        if (it != haContextMap.end()) {
            if (!dialogueId.empty()) it->second.internalDialogueId = dialogueId;
            if (!sessionId.empty())  it->second.internalSessionId  = sessionId;
            return;
        }
        size_t pos = key.rfind('_');
        if (pos == std::string::npos) break;
        key = key.substr(0, pos);
    }
}

// ── 报文构建辅助函数（替代 TEHelper::newUserOutput / newTaskStatusReport） ──

static std::shared_ptr<AccessCtrl> BuildUserOutputMsg(const HaContext& ctx,
    const std::string& outputText, const std::string& streamStatus, int flag)
{
    auto msg = std::make_shared<AccessCtrl>();
    msg->eventType = SYSAGENT::EventType::USEROUTPUT;
    auto h = AisfPlat::MsgAisfHeader{
        .messageType = "DataExchange",
        .userName = ctx.userName,
        .deviceId = ctx.deviceId,
        .teamEnvironmentID = ctx.teamEnvironmentID,
        .deviceCategory = ctx.deviceCategory,
        .uuid = ctx.uuid,
        .clientSessionId = ctx.clientSessionId,
        .acmSessionId = ctx.acmSessionId,
        .eventTimeStamp = AisfPlat::Utils::GetCurrentTimestampInCustomFormat(),
    };
    msg->userOutput.header = h;
    msg->header = h;
    msg->userOutput.data = UserOutput{
        .messageType = "UserOutput",
        .userOutputText = outputText,
        .streamStatus = streamStatus,
        .userOutPutFlag = flag,
    };
    return msg;
}

static std::shared_ptr<AccessCtrl> BuildTaskStatusReportMsg(const HaContext& ctx,
    int code, const std::string& report)
{
    auto msg = std::make_shared<AccessCtrl>();
    msg->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    auto h = AisfPlat::MsgAisfHeader{
        .messageType = "TaskStatusManagement",
        .userName = ctx.userName,
        .deviceId = ctx.deviceId,
        .teamEnvironmentID = ctx.teamEnvironmentID,
        .deviceCategory = ctx.deviceCategory,
        .uuid = ctx.uuid,
        .clientSessionId = ctx.clientSessionId,
        .acmSessionId = ctx.acmSessionId,
        .eventTimeStamp = AisfPlat::Utils::GetCurrentTimestampInCustomFormat(),
    };
    msg->header = h;
    msg->taskStatusManagement.header = h;
    msg->taskStatusManagement.data = TaskStatusManagement{
        .messageType = "TaskStatusReport",
        .taskStatusCode = code,
        .taskStatusReport = report,
    };
    return msg;
}

// ── Helper: HTTP/2 POST 到外部 SysAgent ──

static bool HttpPostToSysAgent(const std::string& sysAgentEntity,
                               const std::shared_ptr<AccessCtrl>& msg)
{
    std::string url = "https://" + sysAgentEntity + ":23767" + USER_REQUEST_URL;
    auto req = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    req->SetMethod(CMFrm::COM::POST);
    req->SetReqUrl(url);
    req->AddHeader("Content-Type", "text/json;charset=utf-8");
    req->SetTimeoutSecond(CMFrm::Constants::Const8);
    req->AddBody(msg->toString());
    std::string resp;
    if (!SYSAGENT::HttpClient::Htt2SendRequest(req, resp)) {
        LOG_ERR_EX("[HealthAgent] HTTP POST failed, url=%s", url.c_str());
        return false;
    }
    return true;
}

// ── 转发函数 ──

// HealthAgent: AgentFlow 响应转发到上游 SysAgent
static CMFrm::COM::HttpStatus ForwardToExternalSysAgent(
    const std::shared_ptr<AccessCtrl>& accessCtrl)
{
    HaContext ctx;
    if (!findHaContext(accessCtrl->header.taskID, ctx)) {
        LOG_ERR_EX("[HealthAgent] Forward: cannot find HaContext for taskID=%s",
                   accessCtrl->header.taskID.c_str());
        return CMFrm::COM::HttpStatus::HTTP400;
    }

    // TASKSTATUSMANAGEMENT → 直接转发到外部 SysAgent
    if (accessCtrl->eventType == SYSAGENT::EventType::TASKSTATUSMANAGEMENT) {
        LOG_INFO_EX("[HealthAgent] Forward: forwarding TaskStatusReport");
        accessCtrl->setSender("HealthAgent");
        accessCtrl->setReceiver("SystemAgent");
        accessCtrl->setTaskID(ctx.parentTaskID);
        HttpPostToSysAgent(ctx.sysAgentEntity, accessCtrl);
        return CMFrm::COM::HttpStatus::HTTP200;
    }

    std::string outputText;
    std::string outputObject;
    std::string streamStatus = "Completed";
    if (accessCtrl->eventType == SYSAGENT::EventType::USEROUTPUT) {
        outputText = accessCtrl->userOutput.data.userOutputText;
        outputObject = accessCtrl->userOutput.data.userOutputObject;
        streamStatus = accessCtrl->userOutput.data.streamStatus;
        if (streamStatus.empty()) streamStatus = "Completed";
    } else {
        LOG_INFO_EX("[HealthAgent] Forward: non-USEROUTPUT eventType=%d", (int)accessCtrl->eventType);
    }

    LOG_INFO_EX("[HealthAgent] Forward: text_len=%zu obj_len=%zu stream=%s eventType=%d",
                outputText.size(), outputObject.size(), streamStatus.c_str(),
                (int)accessCtrl->eventType);

    // 纯透传：coordinator 发什么就转发什么，不自动补发 TaskStatusReport
    // TaskStatusReport 由 coordinator 在 send_final_output 中显式发送
    if (!outputText.empty() || !outputObject.empty()) {
        LOG_INFO_EX("[HealthAgent] Forward: sending UserOutput text=%zu obj=%zu stream=%s",
                    outputText.size(), outputObject.size(), streamStatus.c_str());
        auto userOutput = BuildUserOutputMsg(ctx, outputText, streamStatus, 0);
        userOutput->setSender("HealthAgent");
        userOutput->setReceiver("APP");
        userOutput->setTaskID(ctx.parentTaskID);
        if (!outputObject.empty()) {
            userOutput->userOutput.data.userOutputObject = outputObject;
        }
        HttpPostToSysAgent(ctx.sysAgentEntity, userOutput);
    } else {
        LOG_INFO_EX("[HealthAgent] Forward: SKIP empty text and object, stream=%s", streamStatus.c_str());
    }

    return CMFrm::COM::HttpStatus::HTTP200;
}

// HealthAgent: GUI 桥接报文 HTTP/2 转发到外部 SysAgent
static CMFrm::COM::HttpStatus HandleGuiBridgeForward(
    const std::shared_ptr<AccessCtrl>& accessCtrl)
{
    HaContext ctx;
    if (!findHaContext(accessCtrl->header.taskID, ctx)) {
        LOG_ERR_EX("[HealthAgent] GUI bridge: cannot find HaContext for taskID=%s",
                   accessCtrl->header.taskID.c_str());
        return CMFrm::COM::HttpStatus::HTTP400;
    }

    std::string callbackUrl = "https://" + ctx.sysAgentEntity + ":23767" + USER_REQUEST_URL;
    std::string msgJson = accessCtrl->toString();
    LOG_INFO_EX("[HealthAgent] GUI bridge forward: url=%s len=%zu", callbackUrl.c_str(), msgJson.size());

    auto request = std::make_shared<CMFrm::COM::Http2ClientRequest>();
    request->SetMethod(CMFrm::COM::POST);
    request->SetReqUrl(callbackUrl);
    request->AddHeader("Content-Type", "text/json;charset=utf-8");
    request->SetTimeoutSecond(CMFrm::Constants::Const8);
    request->AddBody(msgJson);

    // ── 记录桥接任务（超时跟踪），在 HTTP 发送前记录，确保 HTTP 失败也跟踪 ──
    RecordGuiBridge(accessCtrl->header.taskID);

    std::string response;
    bool sendResult = SYSAGENT::HttpClient::Htt2SendRequest(request, response);
    if (!sendResult) {
        LOG_ERR_EX("[HealthAgent] GUI bridge forward failed, url=%s", callbackUrl.c_str());
        return CMFrm::COM::HttpStatus::HTTP500;
    }
    LOG_INFO_EX("[HealthAgent] GUI bridge forward success, response=%s", response.c_str());

    return CMFrm::COM::HttpStatus::HTTP200;
}

// HealthAgent: GUI 回调 → 路由到 coordinator
static CMFrm::COM::HttpStatus HandleGuiCallback(
    const std::shared_ptr<AccessCtrl>& accessCtrl)
{
    HaContext ctx;
    if (!findHaContext(accessCtrl->header.taskID, ctx)) {
        LOG_ERR_EX("[HealthAgent] GUI callback: cannot find HaContext for taskID=%s",
                   accessCtrl->header.taskID.c_str());
        return CMFrm::COM::HttpStatus::HTTP400;
    }

    if (ctx.internalDialogueId.empty()) {
        LOG_INFO_EX("[HealthAgent] GUI callback: internal_did EMPTY, routing via default path (skill_selection)");
    } else {
        LOG_INFO_EX("[HealthAgent] GUI callback: internal_did=%s internal_sid=%s → DIRECT route to coordinator",
                    ctx.internalDialogueId.c_str(), ctx.internalSessionId.c_str());
    }
    LOG_INFO_EX("[HealthAgent] GUI callback taskID=%s parentTaskID=%s",
                accessCtrl->header.taskID.c_str(), ctx.parentTaskID.c_str());

    // 设置 clientExField 标识 GUI 回调
    accessCtrl->header.clientExField = "gui_callback";
    accessCtrl->userOutput.header.clientExField = "gui_callback";
    accessCtrl->dataExchange.header.clientExField = "gui_callback";
    accessCtrl->taskStatusManagement.header.clientExField = "gui_callback";

    auto afMgr = SysAgent::AgentFlowManager::GetAgentFlowManager();
    // 使用框架内部 dialogueId/sessionId → 框架 direct routing → 直达 coordinator
    if (!afMgr->SendMsg(accessCtrl, ctx.internalDialogueId, ctx.internalSessionId, HA_AGENT_NAME)) {
        LOG_ERR_EX("[HealthAgent] GUI callback SendMsg failed");
        return CMFrm::COM::HttpStatus::HTTP500;
    }
    LOG_INFO_EX("[HealthAgent] GUI callback routed to coordinator: internal_did=%s", ctx.internalDialogueId.c_str());
    return CMFrm::COM::HttpStatus::HTTP200;
}

// ======================== 核心路由 ========================

// 解析 sender 字段
std::string SysCallBack::ParseSender(const std::shared_ptr<SYSAGENT::AccessCtrl>& access) {
    if (!access) {
        return "Unknown";
    }
    switch (access->eventType) {
        case SYSAGENT::EventType::TASKASSIGNMENT:
            return access->taskAssignment.header.sender;
        case SYSAGENT::EventType::TASKSTOP:
            return access->taskStop.header.sender;
        case SYSAGENT::EventType::USERINPUT:
            return access->userInput.header.sender;
        case SYSAGENT::EventType::USEROUTPUT:
            return access->userOutput.header.sender;
        case SYSAGENT::EventType::DATAEXCHANGE:
            return access->dataExchange.header.sender;
        case SYSAGENT::EventType::TASKSTATUSMANAGEMENT:
            return access->taskStatusManagement.header.sender;
        case SYSAGENT::EventType::LOGINOUT:
            return access->loginOut.header.sender;
        default:
            return "Unknown";
    }
}

// 解析 receiver 字段
std::string SysCallBack::ParseReceiver(const std::shared_ptr<SYSAGENT::AccessCtrl>& access) {
    if (!access) {
        return "Unknown";
    }
    switch (access->eventType) {
        case SYSAGENT::EventType::TASKASSIGNMENT:
            return access->taskAssignment.header.receiver;
        case SYSAGENT::EventType::TASKSTOP:
            return access->taskStop.header.receiver;
        case SYSAGENT::EventType::USERINPUT:
            return access->userInput.header.receiver;
        case SYSAGENT::EventType::USEROUTPUT:
            return access->userOutput.header.receiver;
        case SYSAGENT::EventType::DATAEXCHANGE:
            return access->dataExchange.header.receiver;
        case SYSAGENT::EventType::TASKSTATUSMANAGEMENT:
            return access->taskStatusManagement.header.receiver;
        case SYSAGENT::EventType::LOGINOUT:
            return access->loginOut.header.receiver;
        default:
            return "Unknown";
    }
}

// ── 统一消息路由（精简版，去掉 WorkFlow / ATATask / TE） ──

CMFrm::COM::HttpStatus HandleAgentMsgRequest(const std::shared_ptr<AccessCtrl>& accessCtrl)
{
    auto eventType = accessCtrl->eventType;
    auto sender = accessCtrl->extractSender();
    auto receiver = accessCtrl->extractReceiver();

    LOG_INFO_EX("[HandleAgentMsgRequest] eventType=%d sender=%s receiver=%s taskID=%s",
                static_cast<int>(eventType), sender.c_str(), receiver.c_str(),
                accessCtrl->header.taskID.c_str());

    // 1. GUI 回调：sender=GUIAgent，非 TASKASSIGNMENT → 路由回 coordinator
    if (sender == "GUIAgent" && eventType != SYSAGENT::EventType::TASKASSIGNMENT) {
        // ── GUI 回调超时检查 ──
        if (IsGuiBridgeTimedOut(accessCtrl->header.taskID)) {
            LOG_INFO_EX("[GUI_BRIDGE] STALE_CALLBACK: taskID=%s", accessCtrl->header.taskID.c_str());
            return CMFrm::COM::HttpStatus::HTTP200;
        }
        ClearGuiBridge(accessCtrl->header.taskID);
        return HandleGuiCallback(accessCtrl);
    }

    // 2. 非 TASKASSIGNMENT → AgentFlow 响应，转发到外部 SysAgent
    if (eventType != SYSAGENT::EventType::TASKASSIGNMENT) {
        return ForwardToExternalSysAgent(accessCtrl);
    }

    // 3. TASKASSIGNMENT → 按 receiver 分发
    if (eventType == SYSAGENT::EventType::TASKASSIGNMENT) {
        // 3a. GUI 桥接转发 (receiver=GUIAgent)
        if (receiver == "GUIAgent") {
            return HandleGuiBridgeForward(accessCtrl);
        }

        // 3b. HealthAgent → 直接 SendMsg 进 AgentFlow 框架
        // dialogueId/sessionId 传空，让框架内部生成；桥接回调时再更新
        std::string taskID = accessCtrl->header.taskID;

        // 存储上游上下文，Key = parentTaskID
        // AgentFlow 内部 ID 暂为空，由 SysAgentMsgCallBack 回调更新
        HaContext ctx{
            .sysAgentEntity = accessCtrl->header.sysAgentEntity,
            .parentTaskID = taskID,
            .sender = sender,
            .internalDialogueId = "",
            .internalSessionId = "",
            .userName = accessCtrl->header.userName,
            .deviceId = accessCtrl->header.deviceId,
            .deviceCategory = accessCtrl->header.deviceCategory,
            .uuid = accessCtrl->header.uuid,
            .clientSessionId = accessCtrl->header.clientSessionId,
            .acmSessionId = accessCtrl->header.acmSessionId,
            .teamEnvironmentID = accessCtrl->header.teamEnvironmentID,
        };
        saveHaContext(ctx);

        LOG_INFO_EX("[HealthAgent] TASKASSIGNMENT: SendMsg (empty did/sid) taskID=%s",
                    taskID.c_str());

        auto afMgr = SysAgent::AgentFlowManager::GetAgentFlowManager();
        // 首次传空 dialogueId/sessionId，框架内部自己生成
        if (!afMgr->SendMsg(accessCtrl, "", "", HA_AGENT_NAME)) {
            LOG_ERR_EX("[HealthAgent] AgentFlow SendMsg failed");
            return CMFrm::COM::HttpStatus::HTTP500;
        }
        return CMFrm::COM::HttpStatus::HTTP200;
    }

    return CMFrm::COM::HttpStatus::HTTP200;
}

// ======================== REST 入口 ========================

CMFrm::COM::ServerRespHandleMode SysCallBack::HandleHealthCheck(
        const std::shared_ptr<CMFrm::ServiceRouter::Context>& context)
{
    context->WriteStatueCode(CMFrm::COM::HttpStatus::HTTP200);
    context->WriteResponseBody(R"({"status":"health."})");
    return CMFrm::COM::SYNC;
}

CMFrm::COM::ServerRespHandleMode SysCallBack::HandleUserRequest(
        const std::shared_ptr<CMFrm::ServiceRouter::Context>& context)
{
    std::string reqBody = context->GetRequestBody();
    LOG_INFO_EX("HandleUserRequest, reqBody: %s", reqBody.c_str());

    std::shared_ptr<SYSAGENT::AccessCtrl> accessCtrl = std::make_shared<SYSAGENT::AccessCtrl>();
    std::string errMsg = accessCtrl->Parse(reqBody.c_str(), reqBody.length());
    if (!errMsg.empty()) {
        LOG_ERR("HandleUserRequest parse error: %s", errMsg.c_str());
        context->WriteStatueCode(CMFrm::COM::HttpStatus::HTTP400);
        return CMFrm::COM::SYNC;
    }

    auto retCode = HandleAgentMsgRequest(accessCtrl);
    context->WriteStatueCode(retCode);
    return CMFrm::COM::SYNC;
}

// ======================== AgentFlow 回调 ========================

void SysCallBack::SysAgentMsgCallBack(const shared_ptr<SYSAGENT::AccessCtrl>& access,
    const std::string& dialogueId, const std::string& sessionId)
{
    if (access == nullptr) {
        LOG_ERR("SysAgent ret accessCtrl is nullptr");
        return;
    }
    LOG_INFO("[did=%s] SysAgentMsgCallBack: output=%s sid=%s taskID=%s",
             dialogueId.c_str(),
             access->userOutput.data.userOutputText.c_str(),
             sessionId.c_str(),
             access->header.taskID.c_str());

    // 保存框架内部生成的 dialogueId/sessionId → 按 taskID 匹配 HaContext
    // 桥接转发后回调携带框架内部 ID，后续 GUI 回调可凭此直达 coordinator
    if (!dialogueId.empty()) {
        LOG_INFO("[did=%s] SysAgentMsgCallBack: saving internal IDs sid=%s taskID=%s",
                 dialogueId.c_str(), sessionId.c_str(), access->header.taskID.c_str());
        updateHaContextInternalIds(access->header.taskID, dialogueId, sessionId);
    } else {
        LOG_INFO("[did=%s] SysAgentMsgCallBack: dialogueId empty, skip internal ID update",
                 dialogueId.c_str());
    }

    HandleAgentMsgRequest(access);
}

// ======================== Router 注册 ========================

bool SysCallBack::RegisterRouter()
{
    std::vector<CMFrm::ServiceRouter::UsrServiceRouter> routerList = {
        {CMFrm::COM::GET,  HEALTH_CHECK_URL,          CMFrm::COM::RAW, "default", -1, HandleHealthCheck},
        {CMFrm::COM::POST, USER_REQUEST_URL,           CMFrm::COM::RAW, "default", -1, HandleUserRequest},
        {CMFrm::COM::POST, HEALTH_AGENT_TASK_URL,      CMFrm::COM::RAW, "default", -1, HandleUserRequest},
    };

    routerMgr = std::make_shared<SYSAGENT::TaskRouter>();
    for (const auto& item : routerList) {
        routerMgr->Register(item);
    }
    return true;
}

std::shared_ptr<SYSAGENT::TaskRouter> SysCallBack::GetRouterMgr()
{
    return routerMgr;
}

// ======================== GUI 桥接超时扫描 & 线程 ====================
// 放在文件末尾，可访问上方定义的所有符号（findHaContext / HA_AGENT_NAME 等）

static void GuiTimeoutScan() {
    std::vector<GuiBridgeRecord> timedOut;
    int64_t timeoutMs = GuiTimeoutMs();
    {
        std::lock_guard<std::mutex> lock(g_guiBridgeMutex);
        int64_t now = GuiTimeoutNowMs();
        auto it = g_guiBridgeMap.begin();
        while (it != g_guiBridgeMap.end()) {
            if (now - it->second.sendTimeMs >= timeoutMs) {
                timedOut.push_back(it->second);
                g_guiTimedOutSet.insert(it->first);
                LOG_INFO_EX("[GUI_TIMEOUT] FIRED: parentTaskID=%s elapsed=%lldms",
                    it->first.c_str(), (long long)(now - it->second.sendTimeMs));
                it = g_guiBridgeMap.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& rec : timedOut) {
        HaContext ctx;
        if (!findHaContext(rec.parentTaskID, ctx)) continue;

        // ── 合并帧：TASKSTATUSMANAGEMENT + UserOutputText 一帧发出 ──
        // coordinator 收到后走 merged frame 路径 (gui_text 非空 + task_status>0)
        auto taskStatus = std::make_shared<AccessCtrl>();
        taskStatus->eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
        taskStatus->taskStatusManagement.header.messageType     = "TaskStatusManagement";
        taskStatus->taskStatusManagement.header.sender          = "GUIAgent";
        taskStatus->taskStatusManagement.header.receiver        = ctx.sender;
        taskStatus->taskStatusManagement.header.userName        = ctx.userName;
        taskStatus->taskStatusManagement.header.deviceId        = ctx.deviceId;
        taskStatus->taskStatusManagement.header.deviceCategory  = ctx.deviceCategory;
        taskStatus->taskStatusManagement.header.uuid            = ctx.uuid;
        taskStatus->taskStatusManagement.header.clientSessionId = ctx.clientSessionId;
        taskStatus->taskStatusManagement.header.taskID          = rec.parentTaskID;
        taskStatus->taskStatusManagement.header.clientExField   = "gui_callback";
        taskStatus->taskStatusManagement.data.messageType       = "TaskStatusReport";
        taskStatus->taskStatusManagement.data.taskStatusCode    = 500;
        taskStatus->taskStatusManagement.data.taskStatusReport  = "TaskCompleted";
        taskStatus->taskStatusManagement.data.userOutputText    = "GUI任务执行失败";
        taskStatus->header = taskStatus->taskStatusManagement.header;

        LOG_INFO_EX("[GUI_TIMEOUT] SEND: parentTaskID=%s did=%s sid=%s",
            rec.parentTaskID.c_str(), ctx.internalDialogueId.c_str(), ctx.internalSessionId.c_str());

        auto afMgr = SysAgent::AgentFlowManager::GetAgentFlowManager();
        afMgr->SendMsg(taskStatus, ctx.internalDialogueId, ctx.internalSessionId, HA_AGENT_NAME);
    }
}

static volatile bool g_guiTimeoutRunning = true;

void StartGuiTimeoutThread()
{
    const int64_t checkMs = 30 * 1000;
    LOG_INFO_EX("[GUI_MONITOR] START: timeoutMs=%lld checkMs=%lld",
        (long long)GuiTimeoutMs(), (long long)checkMs);

    std::thread([checkMs]() {
        while (g_guiTimeoutRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(checkMs));
            GuiTimeoutScan();
        }
        LOG_INFO_EX("[GUI_MONITOR] STOP");
    }).detach();
}
