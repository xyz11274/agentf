#ifndef COREMIND_BRIDGE_SIDECAR_ROUTER_H
#define COREMIND_BRIDGE_SIDECAR_ROUTER_H

#include <string>
#include <vector>

// CoreMind ServiceRouter — 仅生产模式下可用（COREMIND_ENABLED=ON）
// stub 模式下该文件不被编译（CMakeLists.txt 控制条件编译）
#ifdef COREMIND_ENABLED
#include <framework_om/service_router/router_manager.h>
#endif

#ifdef COREMIND_ENABLED

/// SidecarRouter — 向 CoreMind ServiceRouter 注册 HTTP 路由。
///
/// 对标 HealthAgent 的 SysCallBack / TaskRouter 模式。
/// 接收上游 CoreMind 框架的 HTTPS HTTP/2 请求，经服务发现后
/// 封装为 trusted envelope，转发给内部 Python Bridge。
class SidecarRouter final : public CMFrm::ServiceRouter::APIRouter {
public:
    /// 注册路由表，供 CoreMind 框架调用。
    std::vector<CMFrm::ServiceRouter::UsrServiceRouter> URLPatterns() override;

    /// 获取 API Provider 名称。
    std::string GetAPIProvider() override;

    /// 注册一条路由到内部列表。
    bool Register(const CMFrm::ServiceRouter::UsrServiceRouter& router);

    // ── 路由处理器 ──

    /// 健康检查
    static CMFrm::COM::ServerRespHandleMode HandleHealthCheck(
        const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

    /// 能力宣告
    static CMFrm::COM::ServerRespHandleMode HandleScheduleTaskAbility(
        const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

    /// 任务入口 — 接收上游 TASKASSIGNMENT，服务发现后转发 Bridge
    static CMFrm::COM::ServerRespHandleMode HandleUserRequest(
        const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

    /// Agent 管理 stub
    static CMFrm::COM::ServerRespHandleMode HandleAgentInstall(
        const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);
    static CMFrm::COM::ServerRespHandleMode HandleAgentUpdate(
        const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);
    static CMFrm::COM::ServerRespHandleMode HandleAgentUninstall(
        const std::shared_ptr<CMFrm::ServiceRouter::Context>& context);

private:
    std::vector<CMFrm::ServiceRouter::UsrServiceRouter> router_list_;

    // ── 配置注入点 ──
    // CoreMind 框架的 handler 签名要求 static（框架通过函数指针调用，不传 this），
    // 所以配置必须用 class static 成员而非实例成员。
    // main.cpp 解析 --bridge-url 后通过 set_bridge_url() 注入，消除对环境变量的隐式依赖。
    // 默认值与原 get_env_or("BRIDGE_URL", "http://127.0.0.1:8199") 一致，向后兼容。
    // 声明在 .h，定义在 .cpp（遵循"头文件只放声明"原则）。
    static std::string bridge_url_;

public:
    /// 配置注入：main.cpp 解析 --bridge-url 后调用。
    /// 必须在 CoreMind framework init 前调用，确保 handler 被回调时配置已就绪。
    static void set_bridge_url(const std::string& url) { bridge_url_ = url; }
};

#endif // COREMIND_ENABLED
#endif // COREMIND_BRIDGE_SIDECAR_ROUTER_H
