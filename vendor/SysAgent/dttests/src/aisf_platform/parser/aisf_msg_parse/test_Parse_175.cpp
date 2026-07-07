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

TEST_F(TestParse, ut_userOutputTextPresent) {
    // Given
    Json::Value root;
    root["UserOutputText"] = "Sample Output";
    string expectedUserOutputText = "Sample Output";
    DataExchange dataExchange;

    // When
    bool result = dataExchange.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(dataExchange.userOutputText, expectedUserOutputText);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ut_userOutputTextMissing) {
    // Given
    Json::Value root;
    string expectedUserOutputText = "";
    DataExchange dataExchange;

    // When
    bool result = dataExchange.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(dataExchange.userOutputText, expectedUserOutputText);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ut_userOutputTextEmpty) {
    // Given
    Json::Value root;
    root["UserOutputText"] = "";
    string expectedUserOutputText = "";
    DataExchange dataExchange;

    // When
    bool result = dataExchange.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(dataExchange.userOutputText, expectedUserOutputText);

    // Clean
    GlobalMockObject::verify();
}

