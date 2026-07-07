///*
// * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
// */
//#include "mockcpp/mockcpp.hpp"
//#include "gtest/gtest.h"
//#include "queue/SystemMessageReadQueueHandler.h"
//#include "TeHelper.h"
//#include "mm_sdk.h"
//#include "data_source.h"
//#include "memory/Location.h"
//
//class TestStart: public ::testing::Test
//{
//protected:
//   static void SetUpTestCase()
//   {
//   }
//
//   static void TearDownTestCase()
//   {
//   }
//
//   virtual void SetUp()
//   {
//   }
//
//   virtual void TearDown()
//   {
//       GlobalMockObject::verify();
//   }
//};
//
//TEST_F(TestStart, start_with_valid_message_and_session) {
//    // Given
//    SystemMessageReadQueueHandler systemMessageReadQueueHandler;
//    QueueMessage msg;
//    msg.data = std::make_shared<SYSAGENT::AccessCtrl>();
//    msg.data->eventType = SYSAGENT::EventType::LOCATION;
//    msg.session = new PerSession();
//    MOCKER_CPP(&BlockingMessageQueue<QueueMessage>::GetMessage, QueueMessage (BlockingMessageQueue<QueueMessage>::*)()).stubs().will(returnValue(msg));
//    MOCKER_CPP(&SystemMessageReadQueueHandler::locationHandler, void (SystemMessageReadQueueHandler::*)(std::shared_ptr<SYSAGENT::AccessCtrl>)).stubs().will(returnValue());
//
//    // When
//    systemMessageReadQueueHandler.start();
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestStart, start_with_invalid_message_data) {
//    // Given
//    SystemMessageReadQueueHandler systemMessageReadQueueHandler;
//    QueueMessage msg;
//    msg.data = nullptr;
//    msg.session = new PerSession();
//    MOCKER_CPP(&BlockingMessageQueue<QueueMessage>::GetMessage, QueueMessage (BlockingMessageQueue<QueueMessage>::*)()).stubs().will(returnValue(msg));
//
//    // When
//    systemMessageReadQueueHandler.start();
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestStart, start_with_null_session_and_recover_success) {
//    // Given
//    SystemMessageReadQueueHandler systemMessageReadQueueHandler;
//    QueueMessage msg;
//    msg.data = std::make_shared<SYSAGENT::AccessCtrl>();
//    msg.data->eventType = SYSAGENT::EventType::LOCATION;
//    msg.session = nullptr;
//    PerSession* recoveredSession = new PerSession();
//    MOCKER_CPP(&BlockingMessageQueue<QueueMessage>::GetMessage, QueueMessage (BlockingMessageQueue<QueueMessage>::*)()).stubs().will(returnValue(msg));
//    MOCKER_CPP(&SystemMessageReadQueueHandler::recoverSessionFromAccessCtrl, PerSession* (SystemMessageReadQueueHandler::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue(recoveredSession));
//    MOCKER_CPP(&SystemMessageReadQueueHandler::locationHandler, void (SystemMessageReadQueueHandler::*)(std::shared_ptr<SYSAGENT::AccessCtrl>)).stubs().will(returnValue());
//
//    // When
//    systemMessageReadQueueHandler.start();
//
//    // Clean
//    GlobalMockObject::verify();
//    delete recoveredSession;
//}
//
//TEST_F(TestStart, start_with_null_session_and_recover_failure) {
//    // Given
//    SystemMessageReadQueueHandler systemMessageReadQueueHandler;
//    QueueMessage msg;
//    msg.data = std::make_shared<SYSAGENT::AccessCtrl>();
//    msg.data->eventType = SYSAGENT::EventType::LOCATION;
//    msg.session = nullptr;
//    MOCKER_CPP(&BlockingMessageQueue<QueueMessage>::GetMessage, QueueMessage (BlockingMessageQueue<QueueMessage>::*)()).stubs().will(returnValue(msg));
//    MOCKER_CPP(&SystemMessageReadQueueHandler::recoverSessionFromAccessCtrl, PerSession* (SystemMessageReadQueueHandler::*)(const std::shared_ptr<SYSAGENT::AccessCtrl>&)).stubs().will(returnValue(nullptr));
//
//    // When
//    systemMessageReadQueueHandler.start();
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestStart, start_with_valid_message_and_invalid_accessCtrl) {
//    // Given
//    SystemMessageReadQueueHandler systemMessageReadQueueHandler;
//    QueueMessage msg;
//    msg.data = nullptr;
//    msg.session = new PerSession();
//    MOCKER_CPP(&BlockingMessageQueue<QueueMessage>::GetMessage, QueueMessage (BlockingMessageQueue<QueueMessage>::*)()).stubs().will(returnValue(msg));
//
//    // When
//    systemMessageReadQueueHandler.start();
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestStart, start_with_location_event_type) {
//    // Given
//    SystemMessageReadQueueHandler systemMessageReadQueueHandler;
//    QueueMessage msg;
//    msg.data = std::make_shared<SYSAGENT::AccessCtrl>();
//    msg.data->eventType = SYSAGENT::EventType::LOCATION;
//    msg.session = new PerSession();
//    MOCKER_CPP(&BlockingMessageQueue<QueueMessage>::GetMessage, QueueMessage (BlockingMessageQueue<QueueMessage>::*)()).stubs().will(returnValue(msg));
//    MOCKER_CPP(&SystemMessageReadQueueHandler::locationHandler, void (SystemMessageReadQueueHandler::*)(std::shared_ptr<SYSAGENT::AccessCtrl>)).stubs().will(returnValue());
//
//    // When
//    systemMessageReadQueueHandler.start();
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestStart, start_with_exception) {
//    // Given
//    SystemMessageReadQueueHandler systemMessageReadQueueHandler;
//    QueueMessage msg;
//    msg.data = std::make_shared<SYSAGENT::AccessCtrl>();
//    msg.data->eventType = SYSAGENT::EventType::LOCATION;
//    msg.session = new PerSession();
//    MOCKER_CPP(&BlockingMessageQueue<QueueMessage>::GetMessage, QueueMessage (BlockingMessageQueue<QueueMessage>::*)()).stubs().will(returnValue(msg));
//    MOCKER_CPP(&SystemMessageReadQueueHandler::locationHandler, void (SystemMessageReadQueueHandler::*)(std::shared_ptr<SYSAGENT::AccessCtrl>)).stubs().will(throwException(std::runtime_error("test exception")));
//
//    // When
//    systemMessageReadQueueHandler.start();
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
