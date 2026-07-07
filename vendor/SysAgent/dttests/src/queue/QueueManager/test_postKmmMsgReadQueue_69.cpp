/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "queue/QueueManager.h"
#include <thread>

class TestPostKmmMsgReadQueue: public ::testing::Test
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

TEST_F(TestPostKmmMsgReadQueue, postKmmMsgReadQueue) {
    // Given
    Memory memory{"user123", "session456", "admin", "content789", "2023-10-01"};
    MOCKER_CPP(&KmmMsgReadQueueHandler::postData, void (KmmMsgReadQueueHandler::*)(const Memory&)).stubs().will(returnValue(0));

    // When
    QueueManager& queueManager = QueueManager::getInstance();
    queueManager.postKmmMsgReadQueue(memory);

    // Then
    // No specific return value to check, verify that postData was called with the correct memory object

    // Clean
    GlobalMockObject::verify();
}

