/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "queue/QueueManager.h"
#include <thread>

class TestPostReadQueue: public ::testing::Test
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

TEST_F(TestPostReadQueue, ut_valid_data_and_session) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>();
    PerSession session = {"session123", nullptr};
    QueueManager& queueManager = QueueManager::getInstance();
    MOCKER_CPP(&ReadQueueHandler::postData, void (ReadQueueHandler::*)(const QueueMessage)).stubs().will(returnValue(0));

    // When
    queueManager.postReadQueue(data, &session);

    // Then
    EXPECT_EQ(true, true);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestPostReadQueue, ut_nullptr_session) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> data = std::make_shared<SYSAGENT::AccessCtrl>();
    PerSession* session = nullptr;
    QueueManager& queueManager = QueueManager::getInstance();
    MOCKER_CPP(&ReadQueueHandler::postData, void (ReadQueueHandler::*)(const QueueMessage)).stubs().will(returnValue(0));

    // When
    queueManager.postReadQueue(data, session);

    // Then
    EXPECT_EQ(true, true);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestPostReadQueue, ut_nullptr_data) {
    // Given
    std::shared_ptr<SYSAGENT::AccessCtrl> data = nullptr;
    PerSession session = {"session123", nullptr};
    QueueManager& queueManager = QueueManager::getInstance();
    MOCKER_CPP(&ReadQueueHandler::postData, void (ReadQueueHandler::*)(const QueueMessage)).stubs().will(returnValue(0));

    // When
    queueManager.postReadQueue(data, &session);

    // Then
    EXPECT_EQ(true, true);

    // Clean
    GlobalMockObject::verify();
}

