/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "taskManage/TaskNode.h"

class TestTaskNode: public ::testing::Test
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

TEST_F(TestTaskNode, ValidTaskPointer) {
    // Given
    std::string taskID = "taskID";
    std::string agentName = "agentName";
    std::string requestContent = "requestContent";
    auto task = std::make_shared<ATATask>(taskID, nullptr, agentName, requestContent);

    // When
    TaskNode taskNode(task);

    // Then
    EXPECT_EQ(taskNode.task, task);

}

TEST_F(TestTaskNode, NullTaskPointer) {
    // Given
    std::shared_ptr<Task> task = nullptr;

    // When
    TaskNode taskNode(task);

    // Then
    EXPECT_EQ(taskNode.task, nullptr);

}
