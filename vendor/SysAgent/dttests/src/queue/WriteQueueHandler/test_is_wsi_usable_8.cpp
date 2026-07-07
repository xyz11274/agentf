/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "queue/WriteQueueHandler.h"
#include "aisf_platform/common_define.h"

class TestIsWsiUsable: public ::testing::Test
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

//TEST_F(TestIsWsiUsable, wsiIsNull) {
//    // Given
//    struct lws *wsi = nullptr;
//
//    // When
//    int ret = is_wsi_usable(wsi);
//
//    // Then
//    EXPECT_EQ(ret, 0);
//
//}
//
//TEST_F(TestIsWsiUsable, wsiNotConnected) {
//    // Given
//    struct lws wsiInstance;
//    struct lws *wsi = &wsiInstance;
//    MOCKER(static_cast<int(*)(struct lws*)>(&lws_get_socket_fd)).stubs().with(eq(wsi)).will(returnValue(-1));
//
//    // When
//    int ret = is_wsi_usable(wsi);
//
//    // Then
//    EXPECT_EQ(ret, 0);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestIsWsiUsable, wsiConnected) {
//    // Given
//    struct lws wsiInstance;
//    struct lws *wsi = &wsiInstance;
//    MOCKER(static_cast<int(*)(struct lws*)>(&lws_get_socket_fd)).stubs().with(eq(wsi)).will(returnValue(1));
//
//    // When
//    int ret = is_wsi_usable(wsi);
//
//    // Then
//    EXPECT_EQ(ret, 1);
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
