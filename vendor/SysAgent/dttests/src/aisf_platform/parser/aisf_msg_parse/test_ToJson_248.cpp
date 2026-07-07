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

TEST_F(TestToJson, ut_default_values) {
    // Given
    LocationInfo locationInfo;
    Json::Value jsonObj;

    // When
    locationInfo.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "LocationInfo");
    EXPECT_EQ(jsonObj["Latitude"].asString(), "");
    EXPECT_EQ(jsonObj["Longitude"].asString(), "");

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestToJson, ut_custom_values) {
    // Given
    LocationInfo locationInfo;
    locationInfo.latitude = "37.7749";
    locationInfo.longitude = "-122.4194";
    Json::Value jsonObj;

    // When
    locationInfo.ToJson(jsonObj);

    // Then
    EXPECT_EQ(jsonObj["MessageType"].asString(), "LocationInfo");
    EXPECT_EQ(jsonObj["Latitude"].asString(), "37.7749");
    EXPECT_EQ(jsonObj["Longitude"].asString(), "-122.4194");

    // Clean
    GlobalMockObject::verify();
}

