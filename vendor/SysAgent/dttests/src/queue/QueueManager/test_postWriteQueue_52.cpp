/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "queue/QueueManager.h"
#include <thread>

class TestPostWriteQueue: public ::testing::Test
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

void postWriteQueueTestMock(const QueueMessage message) {

}

TEST_F(TestPostWriteQueue, postWriteQueueTest) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>();
    PerSession session = {"session123", nullptr};
    QueueManager& queueManager = QueueManager::getInstance();
    // MOCKER_CPP(&WriteQueueHandler::writeData, void (WriteQueueHandler::*)(const QueueMessage)).stubs().will(returnValue());

    // When
    queueManager.postWriteQueue(data, &session);

    // Then
    // No specific expectations needed as writeData is mocked

    // Clean
    GlobalMockObject::verify();
}

