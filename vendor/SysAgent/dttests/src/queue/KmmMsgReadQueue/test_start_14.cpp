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

class TestStart: public ::testing::Test
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

//TEST_F(TestStart, StartWithEmptyQueue) {
//    // Given
//    KmmMsgReadQueueHandler kmmMsgReadQueueHandler;
//    MOCKER_CPP(&BlockingMessageQueue<Memory>::GetMessages, std::vector<Memory> (BlockingMessageQueue<Memory>::*)(size_t, std::chrono::milliseconds)).stubs().will(returnValue(std::vector<Memory>()));
//
//    // When
//    kmmMsgReadQueueHandler.start();
//
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestStart, StartWithNonEmptyQueue) {
//    // Given
//    KmmMsgReadQueueHandler kmmMsgReadQueueHandler;
//    std::vector<Memory> msgs = {Memory(), Memory()};
//    MOCKER_CPP(&BlockingMessageQueue<Memory>::GetMessages, std::vector<Memory> (BlockingMessageQueue<Memory>::*)(size_t, std::chrono::milliseconds)).stubs().will(returnValue(msgs));
//    MOCKER(static_cast<void(*)(std::vector<Memory>&)>(&addKmmMemoryBatchHandler)).stubs().will(returnValue());
//
//    // When
//    kmmMsgReadQueueHandler.start();
//
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestStart, StartWithException) {
//    // Given
//    KmmMsgReadQueueHandler kmmMsgReadQueueHandler;
//    MOCKER_CPP(&BlockingMessageQueue<Memory>::GetMessages, std::vector<Memory> (BlockingMessageQueue<Memory>::*)(size_t, std::chrono::milliseconds)).stubs().willThrow(std::runtime_error("test exception"));
//
//    // When
//    kmmMsgReadQueueHandler.start();
//
//
//    // Clean
//    GlobalMockObject::verify();
//}

