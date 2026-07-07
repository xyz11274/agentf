/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
 */
#include "mockcpp/mockcpp.hpp"
#include "gtest/gtest.h"
#include <securec.h>
#include "memory/SharedMemory.h"
#include "memory/History.h"
#include "utils/http_client.h"
#include "utils/logger.h"
#include "http_client_factory.h"
#include "model/ReactLLMTask.h"
#include "model/PromptManager.h"
#include "config/Config.h"
#include <vector>
#include <map>
#include <sstream>
#include <future>

using namespace mockcpp;

class TestSharedMemoryGetContext : public ::testing::Test {
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


//// 测试场景1: 验证当QuerySharedMemory成功返回数据时，GetContext能够正确地将数据转换并填充到传入的context容器中
//TEST_F(TestSharedMemoryGetContext, ShouldGetContextSuccessfully_WhenQuerySharedMemoryReturnsData) {
//    // Given
//    std::vector<ShortMemory> mockMemoryList = {
//        {"user", "workflow_123", "task_456", "user", "2023-01-01", "source1", "test content 1"},
//        {"assistant", "workflow_456", "task_789", "assistant", "2023-01-02", "source2", "test content 2"}
//    };
//
//    MOCKER(&SharedMemory::QuerySharedMemory).stubs().with(any(), any()).will(invoke([&mockMemoryList](std::vector<ShortMemory>& context, ShortMemory shortMemory) {
//        context = mockMemoryList;
//    }));
//
//    ShortMemory shortMemory;
//    shortMemory.userName = "test_user";
//    shortMemory.workFlowId = "workflow_123";
//    shortMemory.taskId = "task_456";
//    shortMemory.role = "user";
//    shortMemory.content = "test content";
//
//    std::vector<std::map<std::string, std::string>> context;
//
//    // When
//    SharedMemory::GetContext(context, shortMemory);
//
//    // Then
//    ASSERT_EQ(context.size(), 2);
//    EXPECT_EQ(context[0]["role"], "user");
//    EXPECT_EQ(context[0]["content"], "test content 1");
//    EXPECT_EQ(context[1]["role"], "assistant");
//    EXPECT_EQ(context[1]["content"], "test content 2");
//}
//
//static void QuerySharedMemoryMock(std::vector<ShortMemory>& context, ShortMemory shortMemory, std::shared_ptr<std::vector<ShortMemory>> mockMemoryList) {
//    context = *mockMemoryList;
//}

