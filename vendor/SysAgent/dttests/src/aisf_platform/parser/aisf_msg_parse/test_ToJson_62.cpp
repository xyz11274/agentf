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

TEST_F(TestToJson, ut_all_fields_set4) {
    // Given
    MsgAisfHeader msgAisfHeader;
    msgAisfHeader.messageType = "Type1";
    msgAisfHeader.sender = "Sender1";
    msgAisfHeader.receiver = "Receiver1";
    msgAisfHeader.userName = "User1";
    msgAisfHeader.teamEnvironmentID = "Env1";
    msgAisfHeader.deviceId = "Device1";
    msgAisfHeader.deviceCategory = "Category1";
    msgAisfHeader.uuid = "UUID1";
    msgAisfHeader.clientSessionId = "Session1";
    msgAisfHeader.acmSessionId = "ACMSession1";
    msgAisfHeader.taskID = "Task1";
    msgAisfHeader.eventTimeStamp = "Time1";
    msgAisfHeader.sysAgentEntity = "Entity1";
    msgAisfHeader.clientExField = "ExField1";
    Json::Value jsonObj;

    // When
    msgAisfHeader.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "Type1");
    EXPECT_EQ(jsonObj["Sender"].asString(), "Sender1");
    EXPECT_EQ(jsonObj["Receiver"].asString(), "Receiver1");
    EXPECT_EQ(jsonObj["UserName"].asString(), "User1");
    EXPECT_EQ(jsonObj["TeamEnvironmentID"].asString(), "Env1");
    EXPECT_EQ(jsonObj["DeviceId"].asString(), "Device1");
    EXPECT_EQ(jsonObj["DeviceCategory"].asString(), "Category1");
    EXPECT_EQ(jsonObj["UUID"].asString(), "UUID1");
    EXPECT_EQ(jsonObj["ClientSessionId"].asString(), "Session1");
    EXPECT_EQ(jsonObj["AcmSessionId"].asString(), "ACMSession1");
    EXPECT_EQ(jsonObj["TaskID"].asString(), "Task1");
    EXPECT_EQ(jsonObj["EventTimeStamp"].asString(), "Time1");
    EXPECT_EQ(jsonObj["SysAgentEntity"].asString(), "Entity1");
    EXPECT_EQ(jsonObj["ClientExField"].asString(), "ExField1");

}

TEST_F(TestToJson, ut_optional_fields_empty4) {
    // Given
    MsgAisfHeader msgAisfHeader;
    msgAisfHeader.messageType = "Type1";
    msgAisfHeader.sender = "Sender1";
    msgAisfHeader.receiver = "Receiver1";
    msgAisfHeader.userName = "User1";
    msgAisfHeader.deviceId = "Device1";
    msgAisfHeader.deviceCategory = "Category1";
    msgAisfHeader.uuid = "UUID1";
    msgAisfHeader.clientSessionId = "Session1";
    msgAisfHeader.eventTimeStamp = "Time1";
    Json::Value jsonObj;

    // When
    msgAisfHeader.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "Type1");
    EXPECT_EQ(jsonObj["Sender"].asString(), "Sender1");
    EXPECT_EQ(jsonObj["Receiver"].asString(), "Receiver1");
    EXPECT_EQ(jsonObj["UserName"].asString(), "User1");
    EXPECT_EQ(jsonObj["TeamEnvironmentID"].asString(), "");
    EXPECT_EQ(jsonObj["DeviceId"].asString(), "Device1");
    EXPECT_EQ(jsonObj["DeviceCategory"].asString(), "Category1");
    EXPECT_EQ(jsonObj["UUID"].asString(), "UUID1");
    EXPECT_EQ(jsonObj["ClientSessionId"].asString(), "Session1");
    EXPECT_EQ(jsonObj["AcmSessionId"].asString(), "");
    EXPECT_EQ(jsonObj["TaskID"].asString(), "");
    EXPECT_EQ(jsonObj["EventTimeStamp"].asString(), "Time1");
    EXPECT_EQ(jsonObj["SysAgentEntity"].asString(), "");
    EXPECT_EQ(jsonObj["ClientExField"].asString(), "");

}

