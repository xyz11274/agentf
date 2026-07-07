/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/aisf_msg_parse.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat;

class TestParse: public ::testing::Test
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

TEST_F(TestParse, AllMembersPresent) {
    // Given
    Json::Value root;
    root["UserOutputText"] = "Sample Text";
    root["UserOutputObject"] = "Sample Object";
    root["UserManualInterventionFlag"] = true;
    root["ImplicitOutputFlag"] = false;
    root["StreamStatus"] = "Active";
    root["UserOutputFlag"] = 1;
    root["IsVoiceBroadcast"] = 1;

    UserOutput userOutput;

    // When
    bool result = userOutput.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(userOutput.userOutputText, "Sample Text");
    EXPECT_EQ(userOutput.userOutputObject, "Sample Object");
    EXPECT_EQ(userOutput.userManualInterventionFlag, true);
    EXPECT_EQ(userOutput.implicitOutputFlag, false);
    EXPECT_EQ(userOutput.streamStatus, "Active");
    EXPECT_EQ(userOutput.userOutPutFlag, 1);
    EXPECT_EQ(userOutput.isVoiceBroadcast, 1);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, SomeMembersMissing) {
    // Given
    Json::Value root;
    root["UserOutputText"] = "Sample Text";
    root["UserManualInterventionFlag"] = true;
    root["StreamStatus"] = "Active";

    UserOutput userOutput;

    // When
    bool result = userOutput.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(userOutput.userOutputText, "Sample Text");
    EXPECT_EQ(userOutput.userOutputObject, "");
    EXPECT_EQ(userOutput.userManualInterventionFlag, true);
    EXPECT_EQ(userOutput.implicitOutputFlag, false);
    EXPECT_EQ(userOutput.streamStatus, "Active");
    EXPECT_EQ(userOutput.userOutPutFlag, -1);
    EXPECT_EQ(userOutput.isVoiceBroadcast, 0);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, NoMembersPresent) {
    // Given
    Json::Value root;

    UserOutput userOutput;

    // When
    bool result = userOutput.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(userOutput.userOutputText, "");
    EXPECT_EQ(userOutput.userOutputObject, "");
    EXPECT_EQ(userOutput.userManualInterventionFlag, false);
    EXPECT_EQ(userOutput.implicitOutputFlag, false);
    EXPECT_EQ(userOutput.streamStatus, "");
    EXPECT_EQ(userOutput.userOutPutFlag, -1);
    EXPECT_EQ(userOutput.isVoiceBroadcast, 0);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, InvalidTypes) {
    // Given
    Json::Value root;
    root["UserOutputText"] = 123; // Invalid type
    root["UserOutputObject"] = true; // Invalid type
    root["UserManualInterventionFlag"] = "true"; // Invalid type
    root["ImplicitOutputFlag"] = "false"; // Invalid type
    root["StreamStatus"] = 456; // Invalid type
    root["UserOutputFlag"] = "1"; // Invalid type
    root["IsVoiceBroadcast"] = "0"; // Invalid type

    UserOutput userOutput;

    // When
    bool result = userOutput.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(userOutput.userOutputText, "");
    EXPECT_EQ(userOutput.userOutputObject, "");
    EXPECT_EQ(userOutput.userManualInterventionFlag, false);
    EXPECT_EQ(userOutput.implicitOutputFlag, false);
    EXPECT_EQ(userOutput.streamStatus, "");
    EXPECT_EQ(userOutput.userOutPutFlag, -1);
    EXPECT_EQ(userOutput.isVoiceBroadcast, 0);

    // Clean
    GlobalMockObject::verify();
}

