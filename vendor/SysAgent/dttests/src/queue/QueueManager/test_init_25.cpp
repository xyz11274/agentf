/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "queue/QueueManager.h"
#include <thread>

class TestInit: public ::testing::Test
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

//TEST_F(TestInit, ut_startReadQueueHandlers) {
//    // Given
//    MOCKER_CPP(&ReadQueueHandler::start, void (ReadQueueHandler::*)()).stubs().will(returnValue());
//
//    // When
//    QueueManager queueManager;
//    queueManager.init();
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestInit, ut_startWriteQueueHandlers) {
//    // Given
//    MOCKER_CPP(&WriteQueueHandler::start, void (WriteQueueHandler::*)()).stubs().will(returnValue());
//
//    // When
//    QueueManager queueManager;
//    queueManager.init();
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestInit, ut_startSystemMessageReadQueueHandler) {
//    // Given
//    MOCKER_CPP(&SystemMessageReadQueueHandler::start, void (SystemMessageReadQueueHandler::*)()).stubs().will(returnValue());
//
//    // When
//    QueueManager queueManager;
//    queueManager.init();
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestInit, ut_startKmmMsgReadQueueHandler) {
//    // Given
//    MOCKER_CPP(&KmmMsgReadQueueHandler::start, void (KmmMsgReadQueueHandler::*)()).stubs().will(returnValue());
//
//    // When
//    QueueManager queueManager;
//    queueManager.init();
//
//    // Clean
//    GlobalMockObject::verify();
//}

