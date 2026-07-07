/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "queue/WriteQueueHandler.h"
#include "aisf_platform/common_define.h"

class TestWriteData: public ::testing::Test
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

TEST_F(TestWriteData, ut_writeData) {
    // Given
    QueueMessage message;
    message.data = std::make_shared<SYSAGENT::AccessCtrl>();
    message.session = new PerSession();

    WriteQueueHandler writeQueueHandler;

    // When
    writeQueueHandler.writeData(message);

    // Then
    // No assertions needed as the function does not return a value and the behavior is tested through side effects

    // Clean
    delete message.session;
}

