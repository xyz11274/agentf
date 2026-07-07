/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "queue/KmmMsgReadQueueHandler.h"
#include "TeHelper.h"
#include "mm_sdk.h"
#include "data_source.h"
#include "memory/History.h"
#include <thread>
#include <utility>

class TestPostData: public ::testing::Test
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

//TEST_F(TestPostData, postDataWithValidMessage) {
//    // Given
//    Memory message = {"user123", "session456", "admin", "Hello, World!", "2023-10-01"};
//    Mocker_CPP(&BlockingMessageQueue<Memory>::PostMessage, void (BlockingMessageQueue<Memory>::*)(const Memory&)).stubs().will(returnValue());
//
//    // When
//    KmmMsgReadQueueHandler kmmMsgReadQueueHandler;
//    kmmMsgReadQueueHandler.postData(message);
//
//    // Then
//    // No specific assertion needed as PostMessage is mocked
//
//    // Clean
//    GlobalMockObject::verify();
//}

