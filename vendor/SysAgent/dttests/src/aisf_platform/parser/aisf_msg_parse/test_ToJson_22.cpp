/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/aisf_msg_parse.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat;

class TestToJson: public ::testing::Test
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

TEST_F(TestToJson, ut_no_data_no_payload) {
    // Given
    MsgAisfTotal msgAisfTotal;
    msgAisfTotal.header.messageType = "TestType";
    msgAisfTotal.header.sender = "TestSender";
    msgAisfTotal.header.receiver = "TestReceiver";
    msgAisfTotal.header.userName = "TestUser";
    msgAisfTotal.header.teamEnvironmentID = "TestEnv";
    msgAisfTotal.header.deviceId = "TestDevice";
    msgAisfTotal.header.deviceCategory = "TestCategory";
    msgAisfTotal.header.uuid = "TestUUID";
    msgAisfTotal.header.clientSessionId = "TestClientSession";
    msgAisfTotal.header.acmSessionId = "TestAcmSession";
    msgAisfTotal.header.taskID = "TestTask";
    msgAisfTotal.header.eventTimeStamp = "TestTime";
    msgAisfTotal.header.sysAgentEntity = "TestEntity";
    msgAisfTotal.header.clientExField = "TestExField";

    // When
    string result = msgAisfTotal.ToJson();

    // Then
    string expected = "{\"Header\":{\"AcmSessionId\":\"TestAcmSession\",\"ClientExField\":\"TestExField\",\"ClientSessionId\":\"TestClientSession\",\"DeviceCategory\":\"TestCategory\",\"DeviceId\":\"TestDevice\",\"EventTimeStamp\":\"TestTime\",\"MessageType\":\"TestType\",\"Receiver\":\"TestReceiver\",\"Sender\":\"TestSender\",\"SysAgentEntity\":\"TestEntity\",\"TaskID\":\"TestTask\",\"TeamEnvironmentID\":\"TestEnv\",\"UUID\":\"TestUUID\",\"UserName\":\"TestUser\"}}";
    EXPECT_EQ(result, expected);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestToJson, ut_with_data_no_payload) {
    // Given
    MsgAisfTotal msgAisfTotal;
    msgAisfTotal.header.messageType = "TestType";
    msgAisfTotal.header.sender = "TestSender";
    msgAisfTotal.header.receiver = "TestReceiver";
    msgAisfTotal.header.userName = "TestUser";
    msgAisfTotal.header.teamEnvironmentID = "TestEnv";
    msgAisfTotal.header.deviceId = "TestDevice";
    msgAisfTotal.header.deviceCategory = "TestCategory";
    msgAisfTotal.header.uuid = "TestUUID";
    msgAisfTotal.header.clientSessionId = "TestClientSession";
    msgAisfTotal.header.acmSessionId = "TestAcmSession";
    msgAisfTotal.header.taskID = "TestTask";
    msgAisfTotal.header.eventTimeStamp = "TestTime";
    msgAisfTotal.header.sysAgentEntity = "TestEntity";
    msgAisfTotal.header.clientExField = "TestExField";

    Json::Value dataJson;
    dataJson["key"] = "value";
    msgAisfTotal.dataJson = &dataJson;

    // When
    string result = msgAisfTotal.ToJson();

    // Then
    string expected = "{\"Data\":{\"key\":\"value\"},\"Header\":{\"AcmSessionId\":\"TestAcmSession\",\"ClientExField\":\"TestExField\",\"ClientSessionId\":\"TestClientSession\",\"DeviceCategory\":\"TestCategory\",\"DeviceId\":\"TestDevice\",\"EventTimeStamp\":\"TestTime\",\"MessageType\":\"TestType\",\"Receiver\":\"TestReceiver\",\"Sender\":\"TestSender\",\"SysAgentEntity\":\"TestEntity\",\"TaskID\":\"TestTask\",\"TeamEnvironmentID\":\"TestEnv\",\"UUID\":\"TestUUID\",\"UserName\":\"TestUser\"}}";
    EXPECT_EQ(result, expected);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestToJson, ut_no_data_with_payload) {
    // Given
    MsgAisfTotal msgAisfTotal;
    msgAisfTotal.header.messageType = "TestType";
    msgAisfTotal.header.sender = "TestSender";
    msgAisfTotal.header.receiver = "TestReceiver";
    msgAisfTotal.header.userName = "TestUser";
    msgAisfTotal.header.teamEnvironmentID = "TestEnv";
    msgAisfTotal.header.deviceId = "TestDevice";
    msgAisfTotal.header.deviceCategory = "TestCategory";
    msgAisfTotal.header.uuid = "TestUUID";
    msgAisfTotal.header.clientSessionId = "TestClientSession";
    msgAisfTotal.header.acmSessionId = "TestAcmSession";
    msgAisfTotal.header.taskID = "TestTask";
    msgAisfTotal.header.eventTimeStamp = "TestTime";
    msgAisfTotal.header.sysAgentEntity = "TestEntity";
    msgAisfTotal.header.clientExField = "TestExField";

    Json::Value payloadJson;
    payloadJson["key"] = "value";
    msgAisfTotal.payloadJson = &payloadJson;

    // When
    string result = msgAisfTotal.ToJson();

    // Then
    string expected = "{\"Header\":{\"AcmSessionId\":\"TestAcmSession\",\"ClientExField\":\"TestExField\",\"ClientSessionId\":\"TestClientSession\",\"DeviceCategory\":\"TestCategory\",\"DeviceId\":\"TestDevice\",\"EventTimeStamp\":\"TestTime\",\"MessageType\":\"TestType\",\"Receiver\":\"TestReceiver\",\"Sender\":\"TestSender\",\"SysAgentEntity\":\"TestEntity\",\"TaskID\":\"TestTask\",\"TeamEnvironmentID\":\"TestEnv\",\"UUID\":\"TestUUID\",\"UserName\":\"TestUser\"},\"Payload\":{\"key\":\"value\"}}";
    EXPECT_EQ(result, expected);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestToJson, ut_with_data_with_payload) {
    // Given
    MsgAisfTotal msgAisfTotal;
    msgAisfTotal.header.messageType = "TestType";
    msgAisfTotal.header.sender = "TestSender";
    msgAisfTotal.header.receiver = "TestReceiver";
    msgAisfTotal.header.userName = "TestUser";
    msgAisfTotal.header.teamEnvironmentID = "TestEnv";
    msgAisfTotal.header.deviceId = "TestDevice";
    msgAisfTotal.header.deviceCategory = "TestCategory";
    msgAisfTotal.header.uuid = "TestUUID";
    msgAisfTotal.header.clientSessionId = "TestClientSession";
    msgAisfTotal.header.acmSessionId = "TestAcmSession";
    msgAisfTotal.header.taskID = "TestTask";
    msgAisfTotal.header.eventTimeStamp = "TestTime";
    msgAisfTotal.header.sysAgentEntity = "TestEntity";
    msgAisfTotal.header.clientExField = "TestExField";

    Json::Value dataJson;
    dataJson["key"] = "value";
    msgAisfTotal.dataJson = &dataJson;

    Json::Value payloadJson;
    payloadJson["key"] = "value";
    msgAisfTotal.payloadJson = &payloadJson;

    // When
    string result = msgAisfTotal.ToJson();

    // Then
    string expected = "{\"Data\":{\"key\":\"value\"},\"Header\":{\"AcmSessionId\":\"TestAcmSession\",\"ClientExField\":\"TestExField\",\"ClientSessionId\":\"TestClientSession\",\"DeviceCategory\":\"TestCategory\",\"DeviceId\":\"TestDevice\",\"EventTimeStamp\":\"TestTime\",\"MessageType\":\"TestType\",\"Receiver\":\"TestReceiver\",\"Sender\":\"TestSender\",\"SysAgentEntity\":\"TestEntity\",\"TaskID\":\"TestTask\",\"TeamEnvironmentID\":\"TestEnv\",\"UUID\":\"TestUUID\",\"UserName\":\"TestUser\"},\"Payload\":{\"key\":\"value\"}}";
    EXPECT_EQ(result, expected);

    // Clean
    GlobalMockObject::verify();
}

