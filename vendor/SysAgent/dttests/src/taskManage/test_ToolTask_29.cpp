/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include "taskManage/ToolTask.h"
#include <memory>
#include "ruleCognition/CommonDefinition.h"

using namespace mockcpp;
using namespace testing;

class TestToolTask : public ::testing::Test {
protected:
    virtual void SetUp()
    {
    }

    virtual void TearDown()
    {
        GlobalMockObject::verify();
    }

public:
    static void SetUpTestCase()
    {
    }

    static void TearDownTestCase()
    {
    }
};

// UTGen生成测试用例唯一标识符: f629662198964dc4b46aa4e21c11752b
TEST_F(TestToolTask, ShouldInitToolTaskWithCorrectParams_WhenValidInputsProvided) {
    // Given
    std::string taskID = "task001";
    std::string agentName = "agentA";
    std::string requestContent = "request content";

    // When
    ToolTask toolTask(taskID, agentName, requestContent);

    // Then
    EXPECT_EQ(toolTask.taskID, taskID);
    EXPECT_EQ(toolTask.agentName, agentName);
    EXPECT_EQ(toolTask.requestContent, requestContent);
    EXPECT_EQ(toolTask.taskStatus, TaskStatus::PENDING);
}

// UTGen生成测试用例唯一标识符: f310de3f07eb4816ade0abe698ad25e4
TEST_F(TestToolTask, ShouldSetTaskStatusToPending_WhenConstructorCalled) {
    // Given
    std::string taskID = "task002";
    std::string agentName = "agentB";
    std::string requestContent = "another request";

    // When
    ToolTask toolTask(taskID, agentName, requestContent);

    // Then
    EXPECT_EQ(toolTask.taskStatus, TaskStatus::PENDING);
}

// UTGen生成测试用例唯一标识符: a12ad755ab4341e48d6ace991ebd055d
TEST_F(TestToolTask, ShouldAssignCorrectValuesToMembers_WhenStringsAreEmpty) {
    // Given
    std::string taskID = "";
    std::string agentName = "";
    std::string requestContent = "";

    // When
    ToolTask toolTask(taskID, agentName, requestContent);

    // Then
    EXPECT_EQ(toolTask.taskID, taskID);
    EXPECT_EQ(toolTask.agentName, agentName);
    EXPECT_EQ(toolTask.requestContent, requestContent);
    EXPECT_EQ(toolTask.taskStatus, TaskStatus::PENDING);
}

