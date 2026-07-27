// C++ 集成测试骨架——daemon 注入 endpoint 给 workflow node。
#include <gtest/gtest.h>

// TC-SBOX-028: endpoint 未注入返回明确错误，不 fallback 宿主。
// 业务规则: B16 daemon 注入 endpoint，工具侧不获取
// 错误模型: endpoint 未注入——工具侧收到空 endpoint
// 失败意味着: 工具侧静默 fallback 到宿主执行，隔离失效
TEST(SandboxEndpointInjectTest, MissingEndpointReturnsError) {
    GTEST_SKIP() << "实现期驱动——daemon 注入逻辑实现后填充";
}
