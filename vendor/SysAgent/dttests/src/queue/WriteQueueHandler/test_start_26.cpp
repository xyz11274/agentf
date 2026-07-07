/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "queue/WriteQueueHandler.h"
#include "aisf_platform/common_define.h"

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

//TEST_F(TestStart, EmptyMessageData) {
//    // Given
//    WriteQueueHandler writeQueueHandler;
//    QueueMessage msg;
//    msg.data = std::make_shared<std::string>("");
//    MockerCpp(&BlockingMessageQueue<QueueMessage>::GetMessage, QueueMessage (BlockingMessageQueue<QueueMessage>::*)()).stubs().will(returnValue(msg));
//
//    // When
//    writeQueueHandler.start();
//
//    // Then
//    // No assertion needed as the function logs and continues
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestStart, UnusableWsi) {
//    // Given
//    WriteQueueHandler writeQueueHandler;
//    QueueMessage msg;
//    msg.data = std::make_shared<std::string>("testData");
//    msg.session = std::make_shared<Session>();
//    msg.session->wsi = nullptr;
//    MockerCpp(&BlockingMessageQueue<QueueMessage>::GetMessage, QueueMessage (BlockingMessageQueue<QueueMessage>::*)()).stubs().will(returnValue(msg));
//    Mocker(static_cast<int(*)(struct lws*)>(&is_wsi_usable)).stubs().will(returnValue(0));
//
//    // When
//    writeQueueHandler.start();
//
//    // Then
//    // No assertion needed as the function logs and continues
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestStart, UsableWsi) {
//    // Given
//    WriteQueueHandler writeQueueHandler;
//    QueueMessage msg;
//    msg.data = std::make_shared<std::string>("testData");
//    msg.session = std::make_shared<Session>();
//    msg.session->wsi = reinterpret_cast<struct lws*>(0x1);
//    MockerCpp(&BlockingMessageQueue<QueueMessage>::GetMessage, QueueMessage (BlockingMessageQueue<QueueMessage>::*)()).stubs().will(returnValue(msg));
//    Mocker(static_cast<int(*)(struct lws*)>(&is_wsi_usable)).stubs().will(returnValue(1));
//
//    // When
//    writeQueueHandler.start();
//
//    // Then
//    // No assertion needed as the function writes data and continues
//
//    // Clean
//    GlobalMockObject::verify();
//}

