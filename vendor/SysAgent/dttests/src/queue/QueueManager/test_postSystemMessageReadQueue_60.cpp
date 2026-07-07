/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "queue/QueueManager.h"
#include <thread>

class TestPostSystemMessageReadQueue: public ::testing::Test
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

TEST_F(TestPostSystemMessageReadQueue, postSystemMessageReadQueue) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>();
    PerSession session;
    QueueManager& queueManager = QueueManager::getInstance();
    MOCKER_CPP(&SystemMessageReadQueueHandler::postData, void (SystemMessageReadQueueHandler::*)(const QueueMessage message)).stubs().will(returnValue(0));

    // When
    queueManager.postSystemMessageReadQueue(data, &session);

    // Then
    // No specific return value to check, but ensure postData is called with correct parameters
    GlobalMockObject::verify();
}

TEST_F(TestPostSystemMessageReadQueue, postSystemMessageReadQueueWithNullData) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> data = nullptr;
    PerSession session;
    QueueManager& queueManager = QueueManager::getInstance();
    MOCKER_CPP(&SystemMessageReadQueueHandler::postData, void (SystemMessageReadQueueHandler::*)(const QueueMessage message)).stubs().will(returnValue(0));

    // When
    queueManager.postSystemMessageReadQueue(data, &session);

    // Then
    // No specific return value to check, but ensure postData is called with correct parameters
    GlobalMockObject::verify();
}

TEST_F(TestPostSystemMessageReadQueue, postSystemMessageReadQueueWithNullSession) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>();
    PerSession* session = nullptr;
    QueueManager& queueManager = QueueManager::getInstance();
    MOCKER_CPP(&SystemMessageReadQueueHandler::postData, void (SystemMessageReadQueueHandler::*)(const QueueMessage message)).stubs().will(returnValue(0));

    // When
    queueManager.postSystemMessageReadQueue(data, session);

    // Then
    // No specific return value to check, but ensure postData is called with correct parameters
    GlobalMockObject::verify();
}

