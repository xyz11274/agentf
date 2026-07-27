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
};

#endif // COREMIND_ENABLED
#endif // COREMIND_BRIDGE_SIDECAR_ROUTER_H
