// /*
//  * Copyright (c) Huawei Technologies Co., Ltd. 2020-2025. All rights reserved.
//  */
// #include "mockcpp/mockcpp.hpp"
// #include "gtest/gtest.h"
// #include <securec.h>
// #include <iomanip>
// #include <thread>
// #include <filesystem>
// #include <unistd.h>
// #include <sys/sysinfo.h>
// #include "framework_common/utils/net_util.h"
// #include "framework_common/utils/time_util.h"
// #include "aisf_platform/utils/utils.h"
// #include "aisf_platform/common_define.h"

// using namespace mockcpp;
// using namespace AisfPlat;

// class TestGetTimeMs : public ::testing::Test {
// protected:
//     virtual void SetUp()
//     {
//     }

//     virtual void TearDown()
//     {
//         GlobalMockObject::verify();
//     }

// public:
//     static void SetUpTestCase()
//     {
//     }

//     static void TearDownTestCase()
//     {
//     }
// };

// TEST_F(TestGetTimeMs, ShouldReturnValidTimeMs_WhenGettimeofdaySucceeds) {
//     // Given
//     // 无需要mock的外部函数
    
//     // When
//     long result = GetTimeMs();
    
//     // Then
//     EXPECT_GT(result, 0);
// }

// TEST_F(TestGetTimeMs, ShouldReturnZero_WhenGettimeofdayFails) {
//     // Given
//     MOCKER(gettimeofday).stubs().will(returnValue(-1));
    
//     // When
//     long result = GetTimeMs();
    
//     // Then
//     EXPECT_EQ(result, 0);
    
//     // Clean
//     GlobalMockObject::reset();
// }

