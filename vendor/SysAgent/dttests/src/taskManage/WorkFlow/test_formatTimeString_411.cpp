/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2020. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "taskManage/WorkFlow.h"
#include "TeHelper.h"
#include "TeamEnvironment.h"
#include "aisf_platform/parser/aisf_msg_ctrl.h"
#include "aisf_platform/common_macro.h"
#include "queue/QueueManager.h"
#include "taskManage/AgentManager.h"
#include "taskManage/Task.h"
#include "taskManage/TaskEntity.h"
#include "taskManage/TaskNode.h"
#include <cstddef>
#include <string>

class TestFormatTimeString: public ::testing::Test
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

TEST_F(TestFormatTimeString, ut_with_T_and_Z) {
    // Given
    WorkFlow workFlow;
    std::string input = "2023-10-05T14:30:00Z";
    std::string expectedOutput = "2023-10-05 14:30:00";

    // When
    std::string result = workFlow.formatTimeString(input);

    // Then
    EXPECT_EQ(result, expectedOutput);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestFormatTimeString, ut_with_T_without_Z) {
    // Given
    WorkFlow workFlow;
    std::string input = "2023-10-05T14:30:00";
    std::string expectedOutput = "2023-10-05 14:30:00";

    // When
    std::string result = workFlow.formatTimeString(input);

    // Then
    EXPECT_EQ(result, expectedOutput);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestFormatTimeString, ut_without_T_with_Z) {
    // Given
    WorkFlow workFlow;
    std::string input = "2023-10-05 14:30:00Z";
    std::string expectedOutput = "2023-10-05 14:30:00";

    // When
    std::string result = workFlow.formatTimeString(input);

    // Then
    EXPECT_EQ(result, expectedOutput);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestFormatTimeString, ut_without_T_without_Z) {
    // Given
    WorkFlow workFlow;
    std::string input = "2023-10-05 14:30:00";
    std::string expectedOutput = "2023-10-05 14:30:00";

    // When
    std::string result = workFlow.formatTimeString(input);

    // Then
    EXPECT_EQ(result, expectedOutput);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestFormatTimeString, ut_empty_string) {
    // Given
    WorkFlow workFlow;
    std::string input = "";
    std::string expectedOutput = "";

    // When
    std::string result = workFlow.formatTimeString(input);

    // Then
    EXPECT_EQ(result, expectedOutput);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestFormatTimeString, ut_only_T) {
    // Given
    WorkFlow workFlow;
    std::string input = "T";
    std::string expectedOutput = " ";

    // When
    std::string result = workFlow.formatTimeString(input);

    // Then
    EXPECT_EQ(result, expectedOutput);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestFormatTimeString, ut_only_Z) {
    // Given
    WorkFlow workFlow;
    std::string input = "Z";
    std::string expectedOutput = "";

    // When
    std::string result = workFlow.formatTimeString(input);

    // Then
    EXPECT_EQ(result, expectedOutput);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestFormatTimeString, ut_T_and_Z_at_beginning) {
    // Given
    WorkFlow workFlow;
    std::string input = "TZ2023-10-05 14:30:00";
    std::string expectedOutput = " Z2023-10-05 14:30:00";

    // When
    std::string result = workFlow.formatTimeString(input);

    // Then
    EXPECT_EQ(result, expectedOutput);

    // Clean
    GlobalMockObject::verify();
}

TEST_F(TestFormatTimeString, ut_T_and_Z_at_end) {
    // Given
    WorkFlow workFlow;
    std::string input = "2023-10-05 14:30:00TZ";
    std::string expectedOutput = "2023-10-05 14:30:00 ";

    // When
    std::string result = workFlow.formatTimeString(input);

    // Then
    EXPECT_EQ(result, expectedOutput);

    // Clean
    GlobalMockObject::verify();
}

