// C++ 单元测试骨架——IgnoreMatcher（快照跳过 dotfile）。
#include <gtest/gtest.h>

// TC-SBOX-013: 快照跳过 dotfile 只保留交付物。
// 业务规则: B9 跳过 .git/__pycache__/node_modules
// 错误模型: 快照叠加——dotfile 污染快照
// 失败意味着: 快照含 .git 等非交付物，膨胀磁盘占用
TEST(SandboxIgnoreTest, SnapshotSkipsDotfiles) {
    GTEST_SKIP() << "实现期驱动——IgnoreMatcher 实现后填充";
}
