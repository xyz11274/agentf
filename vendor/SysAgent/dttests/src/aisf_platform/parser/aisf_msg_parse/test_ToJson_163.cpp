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

TEST_F(TestToJson, ut_all_fields_set) {
    // Given
    UserOutput userOutput;
    userOutput.userOutputText = "Sample Text";
    userOutput.userOutputObject = "Sample Object";
    userOutput.userManualInterventionFlag = true;
    userOutput.implicitOutputFlag = false;
    userOutput.streamStatus = "Active";
    userOutput.userOutPutFlag = 1;
    userOutput.isVoiceBroadcast = 1;
    Json::Value jsonObj;

    // When
    userOutput.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "UserOutput");
    EXPECT_EQ(jsonObj["UserOutputText"].asString(), "Sample Text");
    EXPECT_EQ(jsonObj["UserOutputObject"].asString(), "Sample Object");
    EXPECT_EQ(jsonObj["UserManualInterventionFlag"].asBool(), true);
    EXPECT_EQ(jsonObj["ImplicitOutputFlag"].asBool(), false);
    EXPECT_EQ(jsonObj["StreamStatus"].asString(), "Active");
    EXPECT_EQ(jsonObj["UserOutputFlag"].asInt(), 1);
    EXPECT_EQ(jsonObj["IsVoiceBroadcast"].asInt(), 1);

}

TEST_F(TestToJson, ut_optional_fields_empty) {
    // Given
    UserOutput userOutput;
    Json::Value jsonObj;

    // When
    userOutput.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "UserOutput");
    EXPECT_EQ(jsonObj["UserOutputText"].asString(), "");
    EXPECT_EQ(jsonObj["UserOutputObject"].asString(), "");
    EXPECT_EQ(jsonObj["UserManualInterventionFlag"].asBool(), false);
    EXPECT_EQ(jsonObj["ImplicitOutputFlag"].asBool(), false);
    EXPECT_EQ(jsonObj["StreamStatus"].asString(), "");
    EXPECT_EQ(jsonObj["UserOutputFlag"].asInt(), -1);
    EXPECT_EQ(jsonObj["IsVoiceBroadcast"].asInt(), 0);

}

