#ifndef MMAGENTGRAPHENGINE_ENGINE_API_H
#define MMAGENTGRAPHENGINE_ENGINE_API_H

#include <string>
#include <functional>
#include <memory>

#include "ir_type_def.h"

namespace mmagentgraphengine {

    struct InitConfig {
        std::string packagePath;        // 存放所有agent包的父目录
    };

    struct ScheduleParam {
        std::string flowId;             // 流程ID
        std::string flowVersion;        // 流程版本
    };

    // 按包名调度的参数（只允许调度Agent类型）
    struct ScheduleByPackageNameParam {
        std::string packageName;        // Agent包名
    };

    struct ActivateParam {
        std::string packageName;        // agent包名，通过遍历irPath+agentDir目录下所有的IR描述文件，完成激活流程
    };

    // 拉远调度代理类型，当前版本未使用
    using RemoteInvoker = std::function<std::string(std::string)>;
    using GraphEngineScheduleHandler = std::function<ErrorCode(const ValueMap& output)>;

    // 引擎初始化
    ErrorCode GraphEngineInit(const InitConfig& cfg, std::shared_ptr<Context> context);

    // Agent调度
    ErrorCode GraphEngineSchedule(const ScheduleParam& param,
                                  const ValueMap& input,
                                  const std::shared_ptr<Context>& context);
    ErrorCode GraphEngineSchedule(const ScheduleParam &param,
                                  const ValueMap& input,
                                  ValueMap& output,
                                  const std::shared_ptr<Context>& context);

    // 按包名调度Agent
    ErrorCode GraphEngineSchedule(const ScheduleByPackageNameParam& param,
                                  const ValueMap& input,
                                  const std::shared_ptr<Context>& context);
    ErrorCode GraphEngineSchedule(const ScheduleByPackageNameParam& param,
                                  const ValueMap& input,
                                  ValueMap& output,
                                  const std::shared_ptr<Context>& context);
    /**
     * 1. 调度执行Agent
     * auto agent = agentflow::framework::MMAgent::create_from_package("/path/to/agent_package");
     */
    ErrorCode GraphEngineScheduleAsync(const ScheduleByPackageNameParam& param,
                                  GraphEngineScheduleHandler& handler,
                                  std::string& agentId);
    ErrorCode GraphEngineSendMsg(const std::string& agentId,
                                  const ValueMap& input);

    // 注入拉远调度代理，当前版本未使用
    ErrorCode InjectRemoteInvoker(const RemoteInvoker& invoker);

    // 激活Agent包
    ErrorCode GraphEngineActivateProcess(const ActivateParam& param);

    // 去激活所有Agent包
    ErrorCode GraphEngineDeactivateProcess();

    // 根据包名去激活Agent
    ErrorCode GraphEngineDeactivateProcess(const ActivateParam& param);
}


#endif // MMAGENTGRAPHENGINE_ENGINE_API_H