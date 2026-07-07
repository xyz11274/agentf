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

class TestAddKmmMemoryBatchHandler: public ::testing::Test
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

void addKmmMemoryBatchHandler_EmptyMessagesMock(BatchMemory& batchMemory) {

}

//TEST_F(TestAddKmmMemoryBatchHandler, addKmmMemoryBatchHandler_EmptyMessages) {
//    // Given
//    std::vector<Memory> msgs;
//    KmmMsgReadQueueHandler kmmMsgReadQueueHandler();
//    MOCKER(&addBatchMemory).stubs().will(invoke(addKmmMemoryBatchHandler_EmptyMessagesMock));
//    // MOCKER_CPP(&addBatchMemory, void (BatchMemory&)).stubs().will(returnValue());
//
//    // When
//    kmmMsgReadQueueHandler.addKmmMemoryBatchHandler(msgs);
//
//    // Then
//    // No assertions needed as the function should just return without errors
//
//    // Clean
//    GlobalMockObject::verify();
//}
//
//TEST_F(TestAddKmmMemoryBatchHandler, addKmmMemoryBatchHandler_NonEmptyMessages) {
//    // Given
//    std::vector<Memory> msgs = {Memory(), Memory()};
//    KmmMsgReadQueueHandler kmmMsgReadQueueHandler;
//    MOCKER_CPP(&addBatchMemory, void (BatchMemory&)).stubs().will(returnValue());
//
//    // When
//    kmmMsgReadQueueHandler.addKmmMemoryBatchHandler(msgs);
//
//    // Then
//    // No assertions needed as the function should just return without errors
//
//    // Clean
//    GlobalMockObject::verify();
//}

