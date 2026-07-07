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

TEST_F(TestParse, ut_messageTypePresent) {
    // Given
    Json::Value root;
    root["MessageType"] = "TestMessage";
    root["Latitude"] = "12.34";
    root["Longitude"] = "56.78";
    LocationInfo locationInfo;

    // When
    bool result = locationInfo.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(locationInfo.messageType, "TestMessage");
    EXPECT_EQ(locationInfo.latitude, "12.34");
    EXPECT_EQ(locationInfo.longitude, "56.78");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ut_messageTypeAbsent) {
    // Given
    Json::Value root;
    root["Latitude"] = "12.34";
    root["Longitude"] = "56.78";
    LocationInfo locationInfo;

    // When
    bool result = locationInfo.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(locationInfo.messageType, "LocationInfo");
    EXPECT_EQ(locationInfo.latitude, "12.34");
    EXPECT_EQ(locationInfo.longitude, "56.78");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ut_latitudeAbsent) {
    // Given
    Json::Value root;
    root["MessageType"] = "TestMessage";
    root["Longitude"] = "56.78";
    LocationInfo locationInfo;

    // When
    bool result = locationInfo.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(locationInfo.messageType, "TestMessage");
    EXPECT_EQ(locationInfo.latitude, "");
    EXPECT_EQ(locationInfo.longitude, "56.78");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ut_longitudeAbsent) {
    // Given
    Json::Value root;
    root["MessageType"] = "TestMessage";
    root["Latitude"] = "12.34";
    LocationInfo locationInfo;

    // When
    bool result = locationInfo.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(locationInfo.messageType, "TestMessage");
    EXPECT_EQ(locationInfo.latitude, "12.34");
    EXPECT_EQ(locationInfo.longitude, "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestParse, ut_allFieldsAbsent) {
    // Given
    Json::Value root;
    LocationInfo locationInfo;

    // When
    bool result = locationInfo.Parse(root);

    // Then
    EXPECT_EQ(result, true);
    EXPECT_EQ(locationInfo.messageType, "LocationInfo");
    EXPECT_EQ(locationInfo.latitude, "");
    EXPECT_EQ(locationInfo.longitude, "");

    // Clean
    GlobalMockObject::verify();
}

