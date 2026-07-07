//
// Created by c00888021 on 2026/1/19.
//
#include "graphManager/base/BaseNode.h"
#include "framework_common/logger/logger.h"
#include "graphManager/GraphUtils.h"
#include "graphManager/GraphManager.h"
#include "taskManage/TeHelper.h"

#define OPERATE_FAILED_CONTENT "系统繁忙，请稍后再试"

using namespace mmagentflow;

MMNodeFunctionStatus BaseNode::ErrorRun(std::shared_ptr<mmagentflow::MMMessage> msg, uint32_t errcode)
{
    auto nodeName = GetName();
    CMFrm_Info("ErrorRun, node name: %s, error code: %u", nodeName.c_str(), errcode);
    std::vector<MMFlowErrorCode> timeoutList = {FUNC_TIMEOUT, BARRIER_TIMEOUT, NODE_RUN_TIMEOUT,
                                                REQUEST_TIMEOUT, FUNC_ERROR};
    for (auto &timeoutErrorCode : timeoutList) {
        if (errcode == static_cast<uint32_t>(timeoutErrorCode)) {
            // 返回固定话术
            auto access = GraphUtils::GetMessageData<AccessCtrlPtr>(msg, GRAPH_DATA_ACCESSCTRL);
            TEHelper::sendUserOutputMsg(access, OPERATE_FAILED_CONTENT, UserOutput_Completed, 0);
            TEHelper::sendNewTaskStatusReport(access);
            return mmagentflow::FUNCTION_COMPLETE;
        }
    }
    return mmagentflow::FUNCTION_COMPLETE;
}

void BaseNode::SetTimeoutTask(std::shared_ptr<mmagentflow::MMMessage> msg, const std::string &timerKey,
                              int timeoutSeconds, std::function<void()> timeoutCallback)
{
    auto timeoutTask = std::make_shared<CMFrm::Timer::TimerTask>(
            timeoutSeconds * CMFrm::Constants::Const1000,
            [msg, timeoutCallback]() {
                timeoutCallback();  // 执行用户传入的回调函数
            }
    );
    CMFrm::Timer::TimerManager::GetTimerManager()->AddTimerTask(timeoutTask);
    GraphUtils::AddMessageData(msg, timerKey, timeoutTask);
}