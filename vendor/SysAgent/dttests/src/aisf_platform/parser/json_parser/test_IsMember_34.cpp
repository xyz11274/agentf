/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "aisf_platform/parser/json_parser.h"

#include "aisf_platform/utils/utils.h"

using namespace AisfPlat::JsonParser;

class TestIsMember: public ::testing::Test
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

TEST_F(TestIsMember, MemberExists) {
    // Given
    Json::Value node;
    node["test"] = 42;
    string member = "test";

    // When
    bool ret = IsMember(node, member);

    // Then
    EXPECT_EQ(ret, true);

}

TEST_F(TestIsMember, MemberDoesNotExist) {
    // Given
    Json::Value node;
    string member = "nonexistent";

    // When
    bool ret = IsMember(node, member);

    // Then
    EXPECT_EQ(ret, false);

}

TEST_F(TestIsMember, ExceptionThrown) {
    // Given
    Json::Value node;
    string member = "test";
    MOCKER(&Json::Value::isMember)  // 直接使用成员函数指针
        .stubs()
        .with(any(), any())
        .will(returnValue(false));
    // When
    bool ret = IsMember(node, member);

    // Then
    EXPECT_EQ(ret, false);

    // Clean
    GlobalMockObject::verify();
}

