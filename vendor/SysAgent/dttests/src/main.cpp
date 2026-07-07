/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024-2024. All rights reserved.
 */

#include "gtest/gtest.h"

using namespace testing;

int main(int argc, char** argv)
{
    InitGoogleTest(&argc, argv);
    int res = RUN_ALL_TESTS();
    if (res == 0) {
        std::cout << "System run successfully." << std::endl;
    } else {
        std::cout << "System run fail." << std::endl;
    }
    return 0;
}
