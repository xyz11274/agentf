/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "taskManage/Task.h"
#include "TeHelper.h"
#include "TeamEnvironment.h"
#include "config/Config.h"
#include "memory/Location.h"
#include "mm_customedreact_llm_component.h"
#include "model/PromptManager.h"
#include "queue/QueueManager.h"
#include "taskManage/QAAgentCallback.h"
#include "taskManage/TeHelper.h"
#include "taskManage/WorkFlow.h"
#include <future>
#include <memory>
#include <utility>
#include "ruleCognition/CommonDefinition.h"
#include "model/ReactLLMTask.h"

class TestBuildMessageBody: public ::testing::Test
{
protected:
   static void SetUpTestCase()
   {
   }

   static void TearDownTestCase()
   {
   }

   virtual void SetUp()
   {
   }

   virtual void TearDown()
   {
       GlobalMockObject::verify();
   }
};

TEST_F(TestBuildMessageBody, TaskAssignment) {
    // Given
    SYSAGENT::AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKASSIGNMENT;
    std::string taskId = "taskid-111";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    ATATask atatask(taskId, nullptr, agentName, requestContent);
    atatask.data.intent = "testIntent";
    atatask.data.params = {{"key1", "value1"}, {"key2", "value2"}};

    // When
    std::string result = atatask.buildMessageBody(accessCtrl);

    // Then
    EXPECT_EQ(result, "{\"Data\":{\"APPName\":\"\",\"MessageType\":\"TaskAssignment\",\"UserInputObject\":\"{\\\"intent\\\":\\\"testIntent\\\",\\\"params\\\":{\\\"key1\\\":\\\"value1\\\",\\\"key2\\\":\\\"value2\\\"}}\",\"UserInputText\":\"\",\"UserIntent\":\"\",\"UserSpecifiedAgent\":\"\"},\"Header\":{\"AcmSessionId\":\"\",\"ClientExField\":\"\",\"ClientSessionId\":\"\",\"DeviceCategory\":\"\",\"DeviceId\":\"\",\"EventTimeStamp\":\"\",\"MessageType\":\"\",\"Receiver\":\"\",\"Sender\":\"SystemAgent\",\"SysAgentEntity\":\"\",\"TaskID\":\"taskid-111\",\"TeamEnvironmentID\":\"\",\"UUID\":\"\",\"UserName\":\"\"}}");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestBuildMessageBody, TaskStop) {
    // Given
    SYSAGENT::AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKSTOP;
    std::string taskId = "taskid-111";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    ATATask atatask(taskId, nullptr, agentName, requestContent);
    // When
    std::string result = atatask.buildMessageBody(accessCtrl);

    // Then
    EXPECT_EQ(result, "{\"Data\":{\"MessageType\":\"TaskStop\",\"UserInputObject\":\"\",\"UserInputText\":\"\",\"UserIntent\":\"\",\"UserSpecifiedAgent\":\"\"},\"Header\":{\"AcmSessionId\":\"\",\"ClientExField\":\"\",\"ClientSessionId\":\"\",\"DeviceCategory\":\"\",\"DeviceId\":\"\",\"EventTimeStamp\":\"\",\"MessageType\":\"\",\"Receiver\":\"\",\"Sender\":\"SystemAgent\",\"SysAgentEntity\":\"\",\"TaskID\":\"taskid-111\",\"TeamEnvironmentID\":\"\",\"UUID\":\"\",\"UserName\":\"\"}}");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestBuildMessageBody, UserInput) {
    // Given
    SYSAGENT::AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::USERINPUT;
    std::string taskId = "taskid-111";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    ATATask atatask(taskId, nullptr, agentName, requestContent);
    // When
    std::string result = atatask.buildMessageBody(accessCtrl);

    // Then
    EXPECT_EQ(result, "{\"Data\":{\"ImplicitInputFlag\":false,\"MessageType\":\"UserInput\",\"UserInputObject\":\"\",\"UserInputText\":\"\",\"UserIntent\":\"\"},\"Header\":{\"AcmSessionId\":\"\",\"ClientExField\":\"\",\"ClientSessionId\":\"\",\"DeviceCategory\":\"\",\"DeviceId\":\"\",\"EventTimeStamp\":\"\",\"MessageType\":\"\",\"Receiver\":\"\",\"Sender\":\"SystemAgent\",\"SysAgentEntity\":\"\",\"TaskID\":\"taskid-111\",\"TeamEnvironmentID\":\"\",\"UUID\":\"\",\"UserName\":\"\"}}");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestBuildMessageBody, UserOutput) {
    // Given
    SYSAGENT::AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::USEROUTPUT;
    std::string taskId = "taskid-111";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    ATATask atatask(taskId, nullptr, agentName, requestContent);
    // When
    std::string result = atatask.buildMessageBody(accessCtrl);

    // Then
    EXPECT_EQ(result, "{\"Data\":{\"ImplicitOutputFlag\":false,\"IsVoiceBroadcast\":0,\"MessageType\":\"UserOutput\",\"StreamStatus\":\"\",\"UserManualInterventionFlag\":false,\"UserOutputFlag\":-1,\"UserOutputObject\":\"\",\"UserOutputText\":\"\"},\"Header\":{\"AcmSessionId\":\"\",\"ClientExField\":\"\",\"ClientSessionId\":\"\",\"DeviceCategory\":\"\",\"DeviceId\":\"\",\"EventTimeStamp\":\"\",\"MessageType\":\"\",\"Receiver\":\"\",\"Sender\":\"SystemAgent\",\"SysAgentEntity\":\"\",\"TaskID\":\"taskid-111\",\"TeamEnvironmentID\":\"\",\"UUID\":\"\",\"UserName\":\"\"}}");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestBuildMessageBody, DataExchange) {
    // Given
    SYSAGENT::AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::DATAEXCHANGE;
    std::string taskId = "taskid-111";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    ATATask atatask(taskId, nullptr, agentName, requestContent);
    // When
    std::string result = atatask.buildMessageBody(accessCtrl);

    // Then
    EXPECT_EQ(result, "{\"Data\":{\"MessageType\":\"DataExchange\",\"UserOutputText\":\"\"},\"Header\":{\"AcmSessionId\":\"\",\"ClientExField\":\"\",\"ClientSessionId\":\"\",\"DeviceCategory\":\"\",\"DeviceId\":\"\",\"EventTimeStamp\":\"\",\"MessageType\":\"\",\"Receiver\":\"\",\"Sender\":\"SystemAgent\",\"SysAgentEntity\":\"\",\"TaskID\":\"taskid-111\",\"TeamEnvironmentID\":\"\",\"UUID\":\"\",\"UserName\":\"\"}}");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestBuildMessageBody, TaskStatusManagement) {
    // Given
    SYSAGENT::AccessCtrl accessCtrl;
    accessCtrl.eventType = SYSAGENT::EventType::TASKSTATUSMANAGEMENT;
    std::string taskId = "taskid-111";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    ATATask atatask(taskId, nullptr, agentName, requestContent);
    // When
    std::string result = atatask.buildMessageBody(accessCtrl);

    // Then
    EXPECT_EQ(result, "{\"Data\":{\"MessageType\":\"\",\"TaskStatusCode\":0,\"TaskStatusReport\":\"\"},\"Header\":{\"AcmSessionId\":\"\",\"ClientExField\":\"\",\"ClientSessionId\":\"\",\"DeviceCategory\":\"\",\"DeviceId\":\"\",\"EventTimeStamp\":\"\",\"MessageType\":\"\",\"Receiver\":\"\",\"Sender\":\"SystemAgent\",\"SysAgentEntity\":\"\",\"TaskID\":\"taskid-111\",\"TeamEnvironmentID\":\"\",\"UUID\":\"\",\"UserName\":\"\"}}");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestBuildMessageBody, DefaultCase) {
    // Given
    SYSAGENT::AccessCtrl accessCtrl;
    accessCtrl.eventType = static_cast<SYSAGENT::EventType>(999); // Invalid event type
    std::string taskId = "taskid-111";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    ATATask atatask(taskId, nullptr, agentName, requestContent);
    // When
    std::string result = atatask.buildMessageBody(accessCtrl);

    std::cout << "-------------------------" << std::endl;
    std::cout << result << std::endl;
    std::cout << "-------------------------" << std::endl;
    // Then
    EXPECT_EQ(result, "");

    // Clean
    GlobalMockObject::verify();
}

