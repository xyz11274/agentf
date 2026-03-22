# 1. 概述

## 1.1 目的

MMAgentRuntime，需要支持插件机制，加载第三方插件包。插件包中提供了常用的服务通信、OM能力（日志打印、告警等）、推理、记忆、工具接口。

## 1.2 范围

本文描述低码化引擎的插件机制、加载流程及已有的插件接口。具体插件so由第三方提供，引擎支持加载。

## 1.3 术语

## 1.4 插件工作流程
【正式方案】
1、	部署方式如下图所示，SysAgent与Worker分离
2、	由Worker中的Server加载Plugin插件，启动UDS通信服务端
3、	Node进程与Server进程之间，通过UDS通信，Node中不提供SDK插件，由Node业务代码直接通过uds，访问插件接口




【临时方案】
1）Worker与SysAgent共进程
2）插件集成在SysAgent中，SysAgent中提供PluginManager，加载插件，并启动plugin
3）Node通过UDS接口访问插件，不提供插件SDK

# 2. 插件机制&加载流程概述

1. 第三方提供插件包，放置在plugins目录
2. Plugin公共接口定义：

```c++
// plugin_manager.h文件中，定义初始的插件及插件管理接口
namespace AgentFrm::Plugin {
    /**
     * 插件定义
     */
    class Plugin {
    public:
        /**
         * 构造函数
         */
        Plugin() = default;

        /**
         * 插件类型
         *
         * @return 插件类型
         */
        virtual std::string GetType() = 0;

        /**
         * 插件名称
         *
         * @return 插件名称
         */
        virtual std::string GetName() = 0;

        /**
         * 插件标签，可以无标签，也可以有多个
         *
         * @return 插件名称
         */
        virtual std::vector<std::string> GetTags() = 0;

        /**
         * 析构函数
         */
        virtual ~Plugin() = default;
    };

    /**
     * 插件管理
     */
    class PluginManager : public CMFrm::Utils::SingletonBase {
    public:
        /**
         * 获取单例
         */
        static std::shared_ptr<PluginManager> GetPluginManager();

        /**
         * 注册插件实例
         *
         * @param plugin 插件实例
         * @return 是否成功
         */
        bool RegisterPluginInstance(const std::shared_ptr<Plugin>& plugin);

        /**
         * 获取插件
         *
         * @param pluginType 插件类型
         * @param tags 标签列表
         * @return 插件对象
         */
        std::pair<std::shared_ptr<Plugin>, bool> GetPlugin(const std::string& pluginType,
            const std::vector<std::string>& tags);

        /**
         * 打印插件信息
         */
        void PrintPluginInfo();
    };
}
```

3. 以MathPlugin.zip为例，说明插件包的格式：

3.1 制作MathPlugin插件包

```
MathPlugin/（为MathPlugin.zip解压后的目录）
├ MathPlugin_description.json
├ include/
├ lib/
└ 3rd/
```

其中MathPlugin_description.json描述了插件名称等信息，libs目录下存放了插件实现库，3rd下存放三方依赖库，include目录中描述了插件对外发布的接口。

3.2 导出插件获取接口和释放接口（对外发布）

````
extern "C" int InitPlugin();  // 获取PluginManager，初始化plugin，并将plugin注册到插件管理器中。返回0表示初始化并注册成功，其它表示失败
````

3.3 实现插件获取接口（内部实现，不对外发布）

```c++
// plugin相关接口实现，不导出，其他模块不感知

class MathPlugin : public Plugin {
    public:
	virtual std::string GetType() = 0;
        virtual int32_t GetPluginName() { return "MathPlugin"; };  // 0: 内部Plugin， 1：对外发布的Plugin
        virtual std::vector<std::string> GetTags() = 0;

	std::string mathAdd(std::string a, std::string b);
    };

extern "C" int InitPlugin();  // 获取PluginManager，初始化plugin，并将plugin注册到插件管理器中
```

4. 对于节点（三方格式为Component）的开发者，可以通过如下方式使用。参考《低码化Agent开发规范》，其中Node的入口函数中，定义了ctx入参，其类型为Context，Context中提供了如下接口：

```c++
class Context {
	shared_ptr<Plugin> getPlugin(const std::string pluginName);
	// ...
};
```

5. XYComponent开发者可以编写如下代码，使用Plugin中提供的接口：

```c++
ErrorCode HandleRequest(const std::shared_ptr<Value> &input, std::shared_ptr<Value> &output, const Context &context) {
	shared_ptr<MathPluginInterface> mathPluginInterface = dynamic_cast<mathPluginInterface>(context.getPluginInterface());
	if (null == mathPluginInterface) {
		cout << "获取插件接口失败"<<endl;
		return -1;
	}
	string addResult = mathPluginInterface->mathAdd("123", "456801231231323122312412312231231231");
	// ……
}
```

# 3. 外部提供的插件接口

## 3.1 LLMPlugin插件

对外发布的接口如下：

```c++
// llmplugin.h
#ifndef MM_LLM_PLUGIN_H
#define MM_LLM_PLUGIN_H

#include <functional>
#include <mutex>
#include "plugin_manager.h"
#include "customedllmcomponent.h"
#include "llmcomponent.h"
#include "utils/class_util.h"

namespace AgentFrm::Plugin {

    typedef uint32_t (*MMWriteLogFunc) (uint32_t level, const std::string& str);
    typedef uint32_t (*MMWriteLogFuncEx) (uint32_t level, const std::string& fileName, int lineNo, const std::string& str);

    /**
    * 日志钩子结构体
    */
    struct MMLLMIntfLogReg {
        std::string modelId;  /**< 日志模块ID */
        uint32_t levelDebug;  /**< debug级别日志 */
        uint32_t levelInfo;  /**< info级别日志 */
        uint32_t levelWarn;  /**< warn级别日志 */
        uint32_t levelError;  /**< error级别日志 */
        uint32_t levelFatal; /**< fatal级别日志 */
        MMWriteLogFunc writeLog{nullptr}; /**< 写文本日志回调函数 */
        MMWriteLogFuncEx writeLogEx{nullptr}; /**< 写文本日志回调函数 */
    };

    class MMLLMIntfInitParam {
    public:
        MMLLMIntfInitParam() : m_logReg{} {}

        // 设置日志初始化参数
        void SetLogReg(const MMLLMIntfLogReg& logReg) {
            m_logReg = logReg;
        }

        // 获取日志初始化参数
        const MMLLMIntfLogReg& GetLogReg() const {
            return m_logReg;
        }

    private:
     MMLLMIntfLogReg m_logReg;
};

    class LLMPlugin : public AgentFrm::Plugin::Plugin, public CMFrm::Utils::SingletonBase {
    public:
     /**
      * 获取单例
      */
     static std::shared_ptr<LLMPlugin> GetInstance() {
         static std::shared_ptr<LLMPlugin> instance(new LLMPlugin());
         return instance;
     }

     std::shared_ptr<MMLlmComponent> CreateMMLlmComponent();

     std::shared_ptr<MMCustomedReactLlmComponent> CreateMMCustomedReactLlmComponent();

     MMCustomedLlmComponentExt& GetMMCustomedLlmComponentExt();

     /**
      * 插件类型
      *
      * @return 插件类型
      */
     std::string GetType() override;

     /**
      * 插件名称
      *
      * @return 插件名称
      */
     std::string GetName() override;

     /**
      * 插件标签，可以无标签，也可以有多个
      *
      * @return 插件名称
      */
     std::vector<std::string> GetTags() override;

     /**
         * 初始化接口
         */
     bool Init(const std::string &configpath, MMLLMIntfLogReg &param);

     /**
      * 析构函数
      */
     ~LLMPlugin() override = default;

    private:
        // 私有构造函数
        LLMPlugin() : m_logFuncEx(nullptr) {}
        MMWriteLogFuncEx m_logFuncEx;   // 扩展日志回调函数指针
    };
}
extern "C" AgentFrm::Plugin::Plugin* CreatePlugin();
extern "C" void DestroyPlugin(AgentFrm::Plugin::Plugin* ptr);

#endif //MM_LLM_PLUGIN_H
```

其中非流式推理的接口定义如下：

```c++
// llmcomponent.h
#ifndef LLMCOMPONENT_H
#define LLMCOMPONENT_H

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <functional>

namespace AgentFrm::Plugin {
    enum MMReasonModalType {
     MMLLM_MODAL_TYPE_TEXT = 0,
     MMLLM_MODAL_TYPE_AUDIO = 1,
     MMLLM_MODAL_TYPE_IMAGE = 2,
    };


    struct MMReasonParam {
     // 模型服务名称：必选
     std::string modelServiceName;
     // 模型实例id：已废弃
     std::string modelInstanceId;
     // 模型名称：必选
     std::string modelName;
     // temperature
     double temperature = 0.1;
     // top_p
     double topP = 0.1;
     // 微调模型id
     std::optional<std::string> loraId = "";
     // 上下文最大长度：可选，单位为字节数；为0表示完全由llm实例的能力来决定，最大长度为32k
     uint32_t contextMaxlength = 0;
     // 调度组标识，0表示默认调度组，完全根据llm负载调度实例信息；
     // 非零取值范围(0, 255],在相同组内的请求在满足负载的条件下会分配到同一个llm实例。
     // 对于同一任务的不同请求，会在同一个llm实例上处理，除非处理失败，才会重新调度到其他实例。
     uint32_t schedgroup = 0;
     // 是否调用第三方模型
     bool useThird = false;
    };

    struct MMUserIntention {
     // 用户输入
     std::string query;

     // 模态类型，0：文本，1：语音，等。预留字段，后续扩展使用。仅文本自动保存上下文，其他模态需要业务自己追加上下文。
     MMReasonModalType modaltype;
    };

    struct MMToolsProperties {
        std::string propertiesType;
        std::string propertiesDescription;
    };

    struct MMToolsParameters {
        // 表示函数参数属性的类型
        std::string parametersType;
        // 函数参数的属性。每一个key表示一个参数名，由用户自定义。value为dict类型，表示参数描述，包含type和description两个参数
        std::map<std::string, MMToolsProperties> properties;
        // 表示函数必填参数列表
        std::vector<std::string> requiredList;
        // 是否允许使用未提及的额外参数 默认值false
        bool additionalProperties = false;
    };

    struct MMPromptTmpltKey {
        // 模板名称，必填,对应json文件中的name字段
        std::string name;
        // 归属，必填
        std::string owner;
        // 语言(可选)，默认zh
        std::string language{"zh"};
        // 版本号(可选)，为空则取最新版本
        std::string version{""};
        // 命名空间(可选)，默认common
        std::string nameSpace{"common"};
    };

    struct MMPromptTemplate {
        MMPromptTmpltKey key;
        // 渲染之后的content
        std::string content;
    };

    struct MMToolsFunction {
        // 函数名称 必选
        std::string functionName;
        // 表示生成tool_calls是否严格遵循schema格式 默认false
        bool strict = false;
        // 描述函数功能和使用
        std::string functionDescription;
        // 表示函数接受的参数
        MMToolsParameters parameters;
    };

    struct MMToolsTemplate {
        // 工具类型 必选 必须为function
        std::string toolsType;
        // 函数描述 必选
        MMToolsFunction toolsFunction;
    };

    struct MMLLMRequest {
        // 用户意图,一次请求一个意图，采用vector方便后续扩展到多模态输入
        // 同一模态的意图只能有一个
        std::vector<MMUserIntention> intentions;

        // 提示模板,需要调用完渲染接口之后再用
        std::vector<MMPromptTemplate> prompt;

        // 可能会使用的工具列表
        std::vector<MMToolsTemplate> tools;
    };

    class MMLlmComponent {
    public:
     /**
      * 单次调用大模型，非流式输出，一问一答，不保存上下文
      *
      * @param param 推理参数
      * @param [INOUT]req 请求
      * @param func 回调函数，第一个入参和返回值都是MMErrorCode
      * @return 返回错误码MMErrorCode
      */
     uint32_t RequestAsync(const MMReasonParam &param, const MMLLMRequest &req,
                           const std::function<uint32_t(uint32_t errorCode, const std::string &response)>& func);
    };
}

#endif //LLMCOMPONENT_H
```

流式推理接口定义如下：

```c++
// customllmcomponent.h
#ifndef CUSTOMEDLLMCOMPONENT_H
#define CUSTOMEDLLMCOMPONENT_H

#include "string"
#include "vector"
#include "memory"
#include "llmcomponent.h"

namespace AgentFrm::Plugin {
    // react任务执行时，业务回调接口
    class MMLLMAsyncCallBack {
    public:
        MMLLMAsyncCallBack() = default;

        virtual ~MMLLMAsyncCallBack() = default;

        /**
         * 大模型流式接口将要开始，但还未调用
         *
         * @param taskId 任务id
         * @param reqId 请求id
         * @return 返回错误码MMErrorCode
         */
        virtual uint32_t OnLLMStart(const std::string &taskId, const std::string &reqId) = 0;

        /**
         * 大模型流式输出结束
         *
         * @param taskId 任务id
         * @param reqId 请求id
         * @return 返回错误码MMErrorCode
         */
        virtual uint32_t OnLLMEnd(const std::string &taskId, const std::string &reqId) = 0;

        /**
         * 大模型流式输出的token
         *
         * @param taskId 任务id
         * @param reqId 请求id
         * @param token 流式token
         * @return 返回错误码MMErrorCode
         */
        virtual uint32_t OnLLMNewToken(
            const std::string &taskId, const std::string &reqId, const std::string &token) = 0;

        /**
         * 大模型流式输出结束之后的完整final answer
         *
         * @param taskId 任务id
         * @param reqId 请求id
         * @param answer 最终回答
         * @return 返回错误码MMErrorCode
         */
        virtual uint32_t OnLLMFinalAnswer(
            const std::string &taskId, const std::string &reqId, const std::string &answer) = 0;

        /**
         * 大模型调用失败
         *
         * @param taskId 任务id
         * @param reqId 请求id
         * @param resultCode 错误码
         * @return 返回错误码MMErrorCode
         */
        virtual uint32_t OnLLMError(
            const std::string &taskId, const std::string &reqId, uint32_t resultCode) = 0;

        uint32_t GetIgnoreLlmBitmap() const { return ignore_llm_bitmp; }

        void SetIgnoreLlmBitmap(uint32_t ignoreLlmBitmp) { ignore_llm_bitmp = ignoreLlmBitmp; }
    protected:
    #define MM_IGNORE_LLM_START (0x1 << 0)
    #define MM_IGNORE_LLM_END (0x1 << 1)
    #define MM_IGNORE_LLM_NEWTOKEN (0x1 << 2)
    #define MM_IGNORE_LLM_FINALANSWER (0x1 << 3)
    #define MM_IGNORE_LLM_ERROR (0x1 << 4)
        uint32_t ignore_llm_bitmp = 0;
    };

    class MMCustomedLlmComponentExt {
    public:
     /**
      * 单例生成MMCustomedLlmComponentExt
      * @return MMCustomedLlmComponentExt
      */
     static MMCustomedLlmComponentExt &GetInstance();
     /**
      * 中断当前会话的Stream请求任务
      *
      * @param taskId 任务id
      * @param func 异步返回结果处理回调，第一个入参和返回值都是MMErrorCode
      * @return 返回错误码MMErrorCode
      */
     uint32_t InterruptStream(const std::string &taskId, const std::function<uint32_t(uint32_t)> &func);

     /**
      * 获取当前任务的全量上下文
      *
      * @param taskId 任务id
      * @param func 异步返回结果处理回调，第一个入参和返回值都是MMErrorCode
      * @return 返回错误码MMErrorCode
      */
     uint32_t GetUserContext(const std::string &taskId,
         const std::function<uint32_t(uint32_t, std::vector<std::map<std::string, std::string>>)> &func);

     /**
      * 更新全量上下文，会丢失工具参数信息
      *
      * @param taskId 任务id
      * @param context 新的上下文
      * @param func 异步返回结果处理回调，第一个入参和返回值都是MMErrorCode
      * @return 返回错误码MMErrorCode
      */
     uint32_t UpdateUserContext(const std::string &taskId,
                                const std::vector<std::map<std::string, std::string>> &context,
                                const std::function<uint32_t(uint32_t)> &func);

     /**
      * 追加上下文
      *
      * @param taskId 任务id
      * @param context 新的上下文
      * @param func 异步返回结果处理回调，第一个入参和返回值都是MMErrorCode
      * @return 返回错误码MMErrorCode
      */
     uint32_t AddUserContext(const std::string &taskId, const std::vector<std::map<std::string, std::string>> &context,
                             const std::function<uint32_t(uint32_t)> &func);
    };

    class MMCustomedReactLlmComponent {
    public:
     /**
      * 创建react动态任务
      *
      * @param param 任务参数
      * @param func 异步返回结果处理回调，第一个入参和返回值都是MMErrorCode
      * @return 返回错误码MMErrorCode
      */
     uint32_t CreateTask(const MMReasonParam &param, std::shared_ptr<MMLLMAsyncCallBack> callback,
                         const std::function<uint32_t(uint32_t errorCode, std::string &taskId)> &func);

     /**
      * 销毁任务
      *
      * @param func 异步返回结果处理回调，第一个入参和返回值都是MMErrorCode
      * @return 返回错误码MMErrorCode
      */
     uint32_t DestroyTask(
             const std::string &taskId,
             const std::function<uint32_t(uint32_t errorCode, std::string taskId)> &func);

     /**
      * 流式请求,为避免上下文混乱，建议对于同一个任务每次只发送一个请求并处理完响应以后再发送下一个请求。如果需要发送多个请求，请使用多个task编程。
      *
      * @param taskId 任务ID
      * @param request 请求
      * @param errorCallback 异步返回超时处理回调，第一个入参和返回值都是MMErrorCode;其他异步响应错误走MMLLMAsyncCallBack::LLMError回调；
      * 成功走MMLLMAsyncCallBack回调
      * @return 返回错误码MMErrorCode
      */
     uint32_t RequestAsync(const std::string &taskId, const MMLLMRequest &request);
    };
}


#endif //CUSTOMEDLLMCOMPONENT_H
```

## 3.2 RAGPlugin插件

对外发布的接口如下：

```c++
// mm_rag_plugin.h 为对外发布的插件接口

#ifndef MM_AGENTRT_RAG_PLUGIN_H
#define MM_AGENTRT_RAG_PLUGIN_H

#include "mm_component_type.h"
#include "datasource/ragcompsdk_data_source.h"
#include "embedding/ragcompsdk_base_embedding.h"
#include "rerank/ragcompsdk_base_rerank.h"
#include "retriever/ragcompsdk_base_retriever.h"
#include "utils/class_util.h"
#include "plugin_manager.h"

namespace AgentFrm::Plugin {

    class RAGPlugin : public AgentFrm::Plugin::Plugin, public CMFrm::Utils::SingletonBase {
    public:
        virtual ~RAGPlugin() = default;

        /**
        *
        * @param modelPtr 模型参数 必选参数
        * @param vocabPtr 词汇表参数 必选参数，暂时不支持任务自定义，后续会支持业务自定义
        * @param inferPtr 推理参数 必选参数
        * @param pattern Embedding 模式：当前只支持本地, 远程还是本地
        * @return 指向`BaseEmbedding`基类的共享指针
        *
        *      const string filePath = "/model/embbedding/gte-small-zh/model.mindir";
        *      const string vocabPath = "/model/embbedding/gte-small-zh/vocab.txt";
        *      const string configPath = "/config/product.json";
        *      auto embedding= BaseEmbedding::CreateEmbedding(
        *          EmbeddingModelOption::Builder(filePath, "bge-small", 512).Build(),
        *          TokenVocabOption::Builder(vocabPath).Build(),
        *          InferenceOption::Builder(DM::RAG::InferenceOption::XpuType::CPU,
        *              DM::RAG::InferenceOption::BackEndType::MINDSPORECLOUD, configPath)
        *              .WithThreadNum(1).WithInstanceNum(1).Build(),
        *          EmbeddingPattern::LOCAL);
        */
        virtual std::shared_ptr<BaseEmbedding> CreateEmbedding(std::shared_ptr<EmbeddingModelOption> modelPtr,
                                                            std::shared_ptr<TokenVocabOption> vocabPtr,
                                                            std::shared_ptr<InferenceOption> inferPtr,
                                                            EmbeddingPattern pattern) = 0;

        virtual std::shared_ptr<BaseReRank> CreateReRank(std::string model_name, ReRankPattern pattern) = 0;

        /**
        * 创建连接池对象, 如果失败将返回空指针
        */
        virtual std::shared_ptr<DataSource> CreateDMDataSource(int appId, const std::string &applicationName,
            const std::string &dbName, const DataSource::Config &config) = 0;

        /**
        * CreateDMDataSource 创建数据库连接池
        *
        * @param appId 服务ID
        * @param applicationName 应用名称
        * @param dbName 数据库名称
        * @return 创建的DataSource对象, 失败则返回空指针
        */
        virtual std::shared_ptr<DataSource> CreateDMDataSource(int appId, const std::string &applicationName,
            const std::string &dbName) = 0;

        virtual std::shared_ptr<BaseRetriever> CreateRetriever(std::shared_ptr<DataSource> dataSource,
            std::shared_ptr<BaseEmbedding> embedding) = 0;
        virtual std::shared_ptr<BaseRetriever> CreateRetriever(std::shared_ptr<DataSource> dataSource,
            std::shared_ptr<BaseEmbedding> embedding, std::shared_ptr<BaseReRank> rerank) = 0;

        // 通过裸指针获取智能指针
        virtual std::shared_ptr<RAGPlugin> GetSharedPtr() = 0;

    protected:
        // 禁止外部创建，但允许子类调用
        RAGPlugin() = default;

    };
}

extern "C" AgentFrm::Plugin::Plugin* CreatePlugin();
extern "C" void DestroyPlugin(AgentFrm::Plugin::Plugin* ptr);

#endif //MM_AGENTRT_RAG_PLUGIN_H

```

datasource目录下存放了数据格式定义
```c++
//datasource/ragcompsdk_data_source.h
```

datasource目录下存放了数据格式定义
```c++
//datasource/ragcompsdk_data_source.h
#ifndef RAGCOMPSDK_EXTERN_DATA_SOURCE_H
#define RAGCOMPSDK_EXTERN_DATA_SOURCE_H

#include <memory>
#include <any>
#include <string>
namespace AgentFrm::Plugin {
constexpr int CONN_INFO_PWD_LEN = 128;
class DataSource {
public:
    class GaussClient;
    struct Config {
        /**
         * 最大数据库连接数
         * 数据范围：(0, 800]
         */
        int maxConnNum = 40;
        /**
         * 初始数据库连接数
         * 数据范围：(0, 800]
         */
        int initConnNum = 5;
        /**
         * 空闲数据库连接数
         * 数据范围：(0, 800]
         */
        int idleConnNum = 3;
        /**
         * 连接回收线程扫描间隔，单位：秒
         * 数据范围：(0, 300]
         */
        int sweepIntervalSec = 30;
        /**
         * 连接最大空闲时间，单位：秒，用于删除非活动连接
         * 数据范围：(0, 300]
         */
        int maxIdleTimeoutSec = 90;
        /**
         * 获取连接时是否检查连接是否存活
         */
        bool testOnBorrow = true;

        // 重载相等运算符
        bool operator==(const Config& other) const {
            return maxConnNum == other.maxConnNum &&
                initConnNum == other.initConnNum &&
                idleConnNum == other.idleConnNum &&
                sweepIntervalSec == other.sweepIntervalSec &&
                maxIdleTimeoutSec == other.maxIdleTimeoutSec &&
                testOnBorrow == other.testOnBorrow;
        }
    };

    virtual ~DataSource() = default;
};

}

#endif  // RAGCOMPSDK_EXTERN_DATA_SOURCE_H
```

embedding目录下存放了embedding相关接口定义
```c++
//embedding/ragcompsdk_base_embedding.h文件
#ifndef RAGCOMPSDK_EXTERN_BASE_EMBEDDING_H
#define RAGCOMPSDK_EXTERN_BASE_EMBEDDING_H

#include <functional>
#include <string>
#include <memory>
#include "embedding/ragcompsdk_embedding_option.h"
namespace AgentFrm::Plugin {
enum class EmbeddingOperateCode : int {
    SUCCESS,
    TIME_OUT = 0x1000001,
    FAIL,
};
struct EmbeddingData {
    std::vector<float> embedding;
    int tokenNum;  // 分词后的token数量
};

enum class EmbeddingPattern { LOCAL, REMOTE };
class BaseEmbedding {
public:
    /**
     * 向量化单个字符串
     * @param query 需要Embedding的字符串
     * @param option Embedding 设置参数
     * @param result Embedding结果
     *
     * @return embedding操作的状态码
     *
     *  string queryStr;
     *  EmbeddingData embeddingData;
     *  ragClient->GetEmbedding()->Embedding(queryStr,
     *      EmbeddingOption::EmbeddingOptionBuilder().WithTimeOut(10).Build(), embeddingData);
     */
    virtual EmbeddingOperateCode Embedding(const std::string &query, std::shared_ptr<EmbeddingOption> option,
                                           EmbeddingData &embeddingData) = 0;

    /**
     * 向量化批量文本
     * @param texts 需要向量化的文本字符串列表
     * @param option Embedding 设置参数
     * @param result Embedding结果
     * @return embedding操作的状态码
     *     vector<string> texts;
     *      vector<EmbeddingData> embeddingDataVec;
     *      ragClient->GetEmbedding()->BatchEmbedding(texts,
     *          EmbeddingOption::EmbeddingOptionBuilder().WithTimeOut(10).Build(), embeddingDataVec);
     *
     */
    virtual EmbeddingOperateCode BatchEmbedding(const std::vector<std::string> &texts,
                                                std::shared_ptr<EmbeddingOption> option,
                                                std::vector<EmbeddingData> &result) = 0;

    virtual ~BaseEmbedding() = default;
};

}

#endif  // RAGCOMPSDK_EXTERN_BASE_EMBEDDING_H
```

```c++
//embedding/ragcompsdk_embedding_option.h
#ifndef RAGCOMPSDK_EXTERN_EMBEDDING_OPTION_H
#define RAGCOMPSDK_EXTERN_EMBEDDING_OPTION_H
#include <memory>
#include <vector>
#include <functional>
namespace AgentFrm::Plugin {
enum class PaddingType {
    AUTO,       // 填充为该批串中的最大长度
    MAX_LENGTH  // 填充为最大长度512
};
class EmbeddingOption {
public:
    virtual ~EmbeddingOption();

public:
    class Builder final {
    public:
        Builder();
        /**
         *
         * @param timeOut 超时时间毫秒
         */

        Builder &WithTimeOut(uint32_t timeOut);
        /**
         *
         * @param paddingType 批量Embedding时填充类型
         */
        Builder &WithPaddingType(PaddingType paddingType);

        std::shared_ptr<EmbeddingOption> Build();

    private:
        class EmbeddingOptionImpl;
        std::shared_ptr<EmbeddingOptionImpl> m_impl;
    };

public:
    virtual uint32_t GetTimeOut() const = 0;
    virtual PaddingType GetPaddingType() const = 0;
};
class EmbeddingModelOption {
public:
    virtual ~EmbeddingModelOption();

public:
    class Builder final {
    public:
        /**
         * 模型要求格式：.mindr, 入参： input_ids， attention_mask， token_type_ids， 出参：last_hidden_state
         * @param modelPath 模型路径
         * @param modelName 模型名称
         * @param modelDim 模型维度
         */
        Builder(const std::string &modelPath, const std::string &modelName, int64_t modelDim);

        std::shared_ptr<EmbeddingModelOption> Build();

    private:
        class EmbeddingModelOptionImpl;
        std::shared_ptr<EmbeddingModelOptionImpl> m_impl;
    };

public:
    virtual const std::string &GetModelPath() const = 0;
    virtual const std::string &GetModelName() const = 0;
    virtual int64_t GetModelDim() const = 0;
};

class TokenVocabOption {
public:
    class Builder final {
    public:
        /**
         * @param vocabPath 本地：代表词汇表的本地文件路径 ；远端：代表词汇表名称
         */
        explicit Builder(const std::string &vocabPath);

        /**
         * @param maxSeqLen 最大序列长度，默认为512
         */
        Builder &WithMaxSeqLen(int maxSeqLen);

        std::shared_ptr<TokenVocabOption> Build();

    private:
        class TokenVocabOptionImpl;
        std::shared_ptr<TokenVocabOptionImpl> m_impl;
    };

public:
    virtual const std::string &GetVocabPath() const = 0;
    virtual int GetMaxSeqLen() const = 0;
};
class InferenceOption {
public:
    enum class XpuType {
        CPU,
        NPU,
    };
    enum class BackEndType { MINDSPORE, MINDSPORECLOUD };

public:
    class Builder final {
    public:
        /**
         *
         * @param xpuType 设备类型 [cpu:0, gpu:1, npu:2],只支持CPU
         * @param backEndType 推理所用的后端，当前仅支持[MindSpore(端侧)，MindSporeCloud(云测)]
         * @param configPath 发布包中config路径下：product.json的路径
         */
        Builder(XpuType xpuType, BackEndType backEndType);

        /**
         * @param threadNum cpu上推理的线程数配置, CPU类型生效，默认1线程，取值范围与推理框架保持一致
         */
        Builder &WithThreadNum(int threadNum);

        /**
         * @param instanceNum 推理的实例个数，默认1实例，建议和执行推理并发线程数保持一致, 取值范围与推理框架保持一致
         */
        Builder &WithInstanceNum(int instanceNum);

        std::shared_ptr<InferenceOption> Build();

    private:
        class InferenceOptionImpl;
        std::shared_ptr<InferenceOptionImpl> m_impl;
    };

public:
    virtual XpuType GetXpuType() = 0;
    virtual std::string GetBackEndType() = 0;
    virtual int GetThreadNum() = 0;
    virtual int GetInstanceNum() = 0;
};
}

#endif  // RAGCOMPSDK_EXTERN_EMBEDDING_OPTION_H
```


rerank目录下存放了rerank格式定义
```c++
//rerank/ragcompsdk_base_rerank.h
#ifndef RAGCOMPSDK_EXTERN_BASE_RERANK_H
#define RAGCOMPSDK_EXTERN_BASE_RERANK_H

#include <string>
#include <memory>
#include <vector>
#include <functional>
#include "rerank/ragcompsdk_rerank_option.h"

namespace AgentFrm::Plugin {
    enum class ReRankOperateCode : int {
        SUCCESS,
        FAIL,
        TIME_OUT = 0x1000001,
    };

    struct ReRankResult {
    public:
        std::string document; // 文本内容
        float relevanceScore; // 相似性得分
    };

    enum class ReRankPattern {
        REMOTE
    };

    class BaseReRank {
    public:
        virtual ReRankOperateCode ReRank(
                const std::string &query, const std::vector<std::string> &searchResult,
                std::shared_ptr<ReRankOption> option, std::vector<ReRankResult> &results) = 0;

        virtual ~BaseReRank() = default;
    };
}

#endif // RAGCOMPSDK_EXTERN_BASE_RERANK_H
```
```c++
// rerank/ragcompsdk_rerank_option.h
#ifndef RAGCOMPSDK_EXTERN_RERANK_OPTION_H
#define RAGCOMPSDK_EXTERN_RERANK_OPTION_H

#include <memory>

namespace AgentFrm::Plugin {
    class ReRankOption {
    public:
        virtual ~ReRankOption();

        class Builder final {
        public:
            Builder();

            /**
            *
            * @param timeOut 超时时间，单位毫秒
            */
            Builder &WithTimeOut(uint32_t timeOut);

            std::shared_ptr<ReRankOption> Build();

        private:
            class ReRankOptionImpl;

            std::shared_ptr<ReRankOptionImpl> m_impl;
        };

        virtual uint32_t GetTimeOut() const = 0;
    };
}

#endif // RAGCOMPSDK_EXTERN_RERANK_OPTION_H
```

retriever目录下存放了retriever数据格式定义
```c++
//retriever/ragcompsdk_base_retriever.h
#ifndef RAGCOMPSDK_EXTERN_BASE_RETRIEVER_H
#define RAGCOMPSDK_EXTERN_BASE_RETRIEVER_H

#include <functional>

#include "string"
#include "retriever/ragcompsdk_table_option.h"
#include "embedding/ragcompsdk_base_embedding.h"
#include "datasource/ragcompsdk_data_source.h"
#include "rerank/ragcompsdk_base_rerank.h"

namespace AgentFrm::Plugin {
enum class DBOperateCode {
    SUCCESS,
    FAIL
};
// BaseRagDB创建需要使用RagManager去创建和管理，不允许单独创建
class BaseRetriever {
public:
    /**
            * 创建数据库schema
            *
            * @param schemaName 创建数据库schema名称
            * @param option  创建schema参数，该参数可以为null
            * @return 创建数据库schema操作是否成功
            *
            * vectorDB->CreateSchema("aaaa", CreateSchemaOption::Builder().WithTimeOut(30).Build());
            */
    virtual DBOperateCode CreateSchema(const std::string &schemaName, std::shared_ptr<CreateSchemaOption> option) = 0;

    /**
             * 删除数据库schema
             *
             * @param schemaName 删除数据库schema
             * @return 删除数据库schema操作是否成功
             *
             * vectorDB->DropSchema("aaaa", DropSchemaOption::Builder().WithTimeOut(60).Build());
             */
    virtual DBOperateCode DropSchema(const std::string &schemaName, std::shared_ptr<DropSchemaOption> option) = 0;

    /**
             * 创建数据库表
             *
             * @param tableName 表名
             * @param option 创建表参数
             * 使用示例：
             *  vectorDB->CreateTable("test",
             *      CreateTableOption::Builder().WithSchemaName("aaaa")
             *          .WithReportIfExists()
             *          .WithFields({
             *              TableFieldProperty::Builder("id", TableFieldType::INT, 64)
             *                  .WithExProperty(ExFieldProperty::Builder().WithPrimaryKey()
             *                  .WithDefaultValue(make_shared<IntergerValue>(10)).Build()).Build(),
             *              TableFieldProperty::Builder("name", TableFieldType::STRING, 64).WithPartition().Build(),
             *              TableFieldProperty::Builder("summary", TableFieldType::STRING, 64)
             *                  .WithExProperty(ExFieldProperty::Builder().WithNotNull() .Build()).Build(),
             *          })
             *          .WithEmbeddingMap({{ "summary", "summary_vector" }})
             *          .Build());
             * @return
             */
    virtual DBOperateCode CreateTable(const std::string &tableName, std::shared_ptr<CreateTableOption> option) = 0;

    /**
             * 修改数据库表结构
             *
             * @param tableName 表名
             * @param option 修改表参数
             * @return
             *  retriever->AlterTable(tableName,AlterTableOption::Builder(AlterTableOption::OpType::ADD,"j")
             *      .WithSchemaName(schemaName).WithFieldProperty(
             *      TableFieldProperty::Builder("j",TableFieldType::INTEGER).Build()
             *  ).Build());
             */
    virtual DBOperateCode AlterTable(const std::string &tableName, std::shared_ptr<AlterTableOption> option) = 0;

    /**
             * 删除数据表结构
             *
             * @param tableName 表名
             * @param option 删除表参数
             * @return
             * DropTable("test",
             *          DropTableOption::Builder().WithSchemaName("public")
             *          .Build());
             */
    virtual DBOperateCode DropTable(const std::string &tableName, std::shared_ptr<DropTableOption> option) = 0;

    /**
             * 创建索引，可加快查询速度
             *
             * @param tableName 表名
             * @param option 创建索引参数
             * @return
             *  retriever->CreateIndex(tableName,
             *      CreateIndexOption::Builder({"t"},"ti",CreateIndexOption::TableIndexType::TEXT)
             *          .WithTextIndexOption(
             *              TextIndexOption::Builder(16).Build()
             *          )
             *          .WithSchemaName(schemaName)
             *          .Build()
             *  )
             */
    virtual DBOperateCode CreateIndex(const std::string &tableName, std::shared_ptr<CreateIndexOption> option) = 0;

    /**
             * DropIndex 删除索引
             *
             * @param indexName index名称
             * @param option 删除索引的参数
             * @return
             *
             * vectorDB.DropIndex("test",
             *      DropIndexOption::DropIndexOptionBuilder()
             *      .WithSchemaName("aaaa")
             *      .Build());
             */
    virtual DBOperateCode DropIndex(const std::string &indexName, std::shared_ptr<DropIndexOption> option) = 0;

    /**
             * Insert 插入单条数据
             *
             * @param tableName 表名
             * @param fields 插入的字段
             * @param values 插入的字段对应的值
             * @param option 插入的参数
             * @return
             *
             *  retriever->Insert(tableName, {"i", "t", "vc", "fv", "ts", "tstz", "b", "ba"},
             *      {make_shared<IntegerValue>(2),
             *      make_shared<TextValue>("text"),
             *      make_shared<VarCharValue>("var char"),
             *      make_shared<FloatVectorValue>(vector<float>{1.,2.,3.}),
             *      make_shared<TimestampValue>(),
             *      make_shared<TimestampTZValue>(),
             *      make_shared<BooleanValue>(true),
             *      make_shared<ByteaValue>(vector<unsigned char>{61,62,63,64}) },
             *      InsertOption::Builder().WithSchemaName(schemaName).Build()
             *  );
             */

    virtual DBOperateCode Insert(const std::string &tableName, const std::vector<std::string> &fields,
                                 const std::vector<std::shared_ptr<TableValue>> &values,
                                 std::shared_ptr<InsertOption> option) = 0;

    /**
             * Update 更新数据
             *
             * @param tableName 需要更新的表名
             * @param updates 需要更新的字段及对应更新值组成的字典。
             * @param option 更新的参数
             * @return
             *
             * vectorDB.Update("test",
             *                  {{"name", "zhangsan"}, {"age", 30},  },
             *                  UpdateOption::Builder()
             *                  .WithFilters(FilterOption::Builder().WithField("i").WithEQ()
             *                      .WithValue(make_shared<IntegerValue>(1)).Build())
             *                  .Build());
             */
    virtual DBOperateCode Update(const std::string &tableName,
                                 const std::map<std::string, std::shared_ptr<TableValue>> &updates,
                                 std::shared_ptr<UpdateOption> option) = 0;

    /**
             * Delete 删除数据
             *
             * @param tableName 表名
             * @param option 删除表的参数
             * @return
             *
             *  vectorDB.Delete("test",
             *      DeleteOption::Builder()
             *                  .WithSchemaName("aaaa")
             *                  .WithFilters(FilterOption::Builder()
             *                      .WithField("id")
             *                      .WithGT()
             *                      .WithValue(make_shared<IntegerValue>(1))
             *                      .WithAND()
             *                      .WithField("id")
             *                      .WithLT()
             *                      .WithValue(make_shared<IntegerValue>(1000))
             *                      .Build())
             *                  .Build());
             */
    virtual DBOperateCode Delete(const std::string &tableName, std::shared_ptr<DeleteOption> option) = 0;

    /**
             * BatchInsert 批量插入
             * @param tableName 表名
             * @param fields 插入的字段名
             * @param values 插入的值, value需要使用rag_db_param参数转换为string后传入
             * @param option 插入的参数
             * @return
             *
             *  retriever->BatchInsert(tableName, {"i","t","v"},
             *      {
             *          {make_shared<IntegerValue>(1),
             *              make_shared<TextValue>("2"),
             *              make_shared<FloatVectorValue>(vector<float>{1.,2.,3.})},
             *          {make_shared<IntegerValue>(1),
             *              make_shared<TextValue>("2"),
             *              make_shared<FloatVectorValue>(vector<float>{1.,2.,3.})},
             *          {make_shared<IntegerValue>(1),
             *              make_shared<TextValue>("2"),
             *              make_shared<FloatVectorValue>(vector<float>{1.,2.,3.})},
             *      },
             *      InsertOption::Builder().WithSchemaName(schemaName).Build()
             *  )
             */
    virtual DBOperateCode BatchInsert(const std::string &tableName, const std::vector<std::string> &fields,
                                      const std::vector<std::vector<std::shared_ptr<TableValue>>> &values,
                                      std::shared_ptr<InsertOption> option) = 0;

    /**
             * ScalarSearch
             *
             * @param tableName 表名
             * @param option 查询参数
             * @param result 查询结果
             * @return
             *
             *  retriever->ScalarSearch(tableName,
             *      QueryOption::Builder(TableQueryType::SCALAR)
             *          .WithBaseGroup(groupName)
             *          .WithBaseSchemaName(schemaName)
             *          .WithBaseSelectFields(selectFields)
             *          .WithBaseOrderField(orderField)
             *          .WithBaseOrder(QueryOption::QueryOrder::ASC)
             *          .WithBaseTimeOut(timeout)
             *          .WithScalarLimit(limit)
             *          .WithScalarOffset(offset)
             *          .Build(),
             *  result);
             */
    virtual DBOperateCode ScalarSearch(const std::string &tableName, std::shared_ptr<QueryOption> option,
                                       std::vector<std::vector<std::string>> &result) = 0;

    /**
             * VectorSearch 向量搜素
             *
             * @param tableName 表名
             * @param option 查询参数
             * @param result 查询结果
             * @return
             *
             * retriever->VectorSearch(tableName,
             *     QueryOption::Builder(TableQueryType::VECTOR)
             *         .WithBaseGroup(groupname)
             *         .WithBaseSchemaName(schemaname)
             *         .WithBaseSelectFields(selectfields)
             *         .WithBaseOrder(QueryOption::QueryOrder::ASC)
             *         .WithBaseTimeOut(timeout)
             *         .WithVectorField(vectorField)
             *         .WithVectorDistanceAlgorithm(VectorDistanceAlgorithm::COSIN)
             *         .WithVectorIndex(vectorIndex)
             *         .WithVectorTopK(vectorTopK)
             *         .WithVectorQuery(vectorQuery)
             *         .WithVectorReturnDistance()
             *         .WithVectorDistanceThreshold(1.0)
             *         .Build(),
             * result);
             */
    virtual DBOperateCode VectorSearch(const std::string &tableName, std::shared_ptr<QueryOption> option,
                                       std::vector<std::vector<std::string>> &result) = 0;

    /**
             * TextSearch 文本搜索
             *
             * @param tableName 表名
             * @param option 查询参数
             * @param result 查询结果
             * @return
             *
             * retriever->TextSearch(tableName,
             *     QueryOption::Builder(TableQueryType::TEXT)
             *         .WithBaseGroup(groupName)
             *         .WithBaseSchemaName(schemaName)
             *         .WithBaseSelectFields(selectFields)
             *         .WithBaseOrder(QueryOption::QueryOrder::ASC)
             *         .WithBaseTimeOut(timeout)
             *         .WithTextField(textField)
             *         .WithTextIndex(textIndex)
             *         .WithTextTopK(textTopK)
             *         .WithTextQuery(textQuery)
             *         .WithTextReturnScore()
             *         .WithTextScoreThreshold(1.0)
             *         .Build(),
             * result);
             */
    virtual DBOperateCode TextSearch(const std::string &tableName, std::shared_ptr<QueryOption> option,
                                     std::vector<std::vector<std::string>> &result) = 0;


    /**
             * FusionSearch 混合检索接口
             *  1. WithTextTopK和WithVecorTopK无效，请使用WithFusionTopK
             *  2. WithTextQuery和WithVectorQuery无效，请使用WithFusionQuery
             * @param tableName 表名
             * @param option 查询参数
             * @param result 向量距离与文本得分都设置： 第一列：向量的距离， 第二：文本的得分;如果只设置单个，则第一列为对应的向量距离或者文本得分; 如果没有设计返回得分或距离，则直接中返回的列值开发
             * @return
             *
             *     vectorDB->FusionSearch("test",
                        QueryOption::QueryOptionBuilder(TableQueryType::FUSION)
                        .WithFusionQuery("ccccc")
                        .WithFusionTopK(5)
                        .WithFusionMethod(QueryOption::FusionMethod::RRF)
                        .WithFusionRRFK(30)
                        .WithVectorField("summary_vector")
                        .WithVectorIndex("idx_part_summary_vector")
                        .WithVectorDistanceAlgorithm(VectorDistanceAlgorithm::L2)
                        .WithTextField("summary")
                        .WithTextIndex("idx_part_summary")
                        .Build(), searchResult);
             */
    virtual DBOperateCode FusionSearch(const std::string &tableName, std::shared_ptr<QueryOption> option,
                                       std::vector<std::vector<std::string>> &result) = 0;

    /**
             * GraphSearch 知识图谱搜索
             *
             * @param graphName 图空间名
             * @param option 查询参数
             * @param result 查询结果
             * @return
             *
             * retriever->GraphSearch(graphName,
             *     QueryOption::Builder(TableQueryType::TEXT)
             *         .WithBaseTimeOut(timeout)
             *         .WithTextQuery(textQuery)
             *         .Build(),
             * result);
             */
    virtual DBOperateCode GraphSearch(const std::string &graphName, std::shared_ptr<QueryOption> option,
                                     std::vector<std::vector<std::string>> &result) = 0;

    /**
             * Exec 执行sql语句
             *
             * @param sql 需要执行的sql语句
             * @return 执行sql语句操作是否成功
             */
    virtual DBOperateCode Exec(const std::string &sql, std::shared_ptr<SqlExecOption> option) = 0;

    /**
            * ScalarSearch 执行sql语句
            *
            * @param sql 需要查询的sql语句
            * @param result 执行sql语句返回的数据列表
            * @return 执行sql语句操作是否成功
            */
    virtual DBOperateCode Query(const std::string &sql, std::shared_ptr<SqlQueryOption> option,
                                std::vector<std::vector<std::string>> &result) = 0;


    /**
             * 判断表是否存在
             * @param tableName 表名
             * @param schema schema名，默认为public
             * @param result 返回结果
             */
    virtual DBOperateCode IsTableExist(const std::string &tableName,
        bool &result, const std::string &schema = "public") = 0;
    /**
             * 判断schema是否存在
             * @param tableName
             * @param result 返回结果
             */
    virtual DBOperateCode IsSchemaExist(const std::string &schema, bool &result) = 0;

    virtual ~BaseRetriever() = default;
};

}
#endif  // RAGCOMPSDK_EXTERN_BASE_RETRIEVER_H
```

```c++
//retriever/ragcompsdk_table_option.h
#ifndef RAGCOMPSDK_EXTERN_TABLE_OPTION_H
#define RAGCOMPSDK_EXTERN_TABLE_OPTION_H
#include <variant>
#include <memory>
#include <vector>
#include <string>
#include <map>
#include "retriever/ragcompsdk_table_value.h"

namespace AgentFrm::Plugin {

enum class TableFieldType {
    IsSetTableFieldType,
    SERIAL,     // 自增序列
    INTEGER,
    VARCHAR,  // 需要指定长度
    BYTEA,
    TEXT,
    BOOLEAN,
    TIMESTAMP,    // 不带时区 UTC时间, 默认时区为0
    TIMESTAMPTZ,  // 带时区
    FLOATVECTOR   // 需要制定长度
};
class ExFieldProperty {
public:
    virtual ~ExFieldProperty();

public:
    class Builder final {
    public:
        Builder();
        /**
         * @param isPrimaryKey 字段是否为主键
         * @param isNotNull 字段值是否不为空
         * @param isUnique 字段值是否唯一
         * @param defaultValue 字段值的默认值
         */
        explicit Builder(bool isPrimaryKey, bool isNotNull, bool isUnique, std::shared_ptr<TableValue> defaultValue);
        /**
         * 设置字段为主键，非必选字段
         */
        Builder &WithPrimaryKey();

        /**
         * 设置字段NotNull，非必选字段
         */
        Builder &WithNotNull();
        /**
         * 设置字段Unique，非必选字段
         */
        Builder &WithUnique();

        /**
         * 设置字段默认值
         * @param defaultValue 字段默认值，非必选字段
         */
        Builder &WithDefaultValue(std::shared_ptr<TableValue> defaultValue);
        std::shared_ptr<ExFieldProperty> Build();

    private:
        class ExFieldPropertyImpl;
        std::shared_ptr<ExFieldPropertyImpl> m_impl;
    };

public:
    virtual bool IsPrimaryKey() const = 0;
    virtual bool IsNotNull() const = 0;
    virtual bool IsUnique() const = 0;
    virtual const std::shared_ptr<TableValue> &GetDefaultValue() const = 0;
};

class TableFieldProperty {
public:
    virtual ~TableFieldProperty();

public:
    class Builder final {
    public:
        /**
         * @param fieldName 字段名称
         * @param fieldType 字段类型
         */

        Builder(const std::string &fieldName, TableFieldType fieldType);
        /**
         * @param fieldName 字段名称
         * @param fieldType 字段类型
         * @param length 字段长度
         */
        Builder(const std::string &fieldName, TableFieldType fieldType, int length);

        Builder(const std::string &fieldName, TableFieldType fieldType, int length, bool isPartition,
                const std::shared_ptr<ExFieldProperty> &exProperty);

        /**
         * 设置为分区字段，默认为不分区，非必选字段, 目前分区字段类型仅支持INTEGER
         */
        Builder &WithPartition();
        /**
         * @param exProperty 字段扩展属性，非必选字段
         */
        Builder &WithExProperty(std::shared_ptr<ExFieldProperty> exProperty);

        std::shared_ptr<TableFieldProperty> Build();

    private:
        class TableFieldPropertyImpl;
        std::shared_ptr<TableFieldPropertyImpl> m_impl;
    };

public:
    virtual const std::string &GetFieldName() const = 0;
    virtual TableFieldType GetFieldType() const = 0;
    virtual int GetLength() const = 0;
    virtual bool IsPartition() const = 0;
    virtual const std::shared_ptr<ExFieldProperty> &GetExProperty() const = 0;
};
class CreateTableOption {
public:
    virtual ~CreateTableOption();

public:
    class Builder final {
    public:
        Builder();
        /**
         * @param schemaName 指定创建表所属的schema，默认为public，非必选字段
         */
        Builder &WithSchemaName(const std::string &schemaName);
        /**
         * @param reportIfExists 表存在时是否去创建，默认为true，非必选字段
         *                        true: 如果表存在，报错且不创建表，表不存在，创建表不报错
         *                        false: 如果表存在，不报错且不创建表，表不存在，创建表不报错
         */
        Builder &WithReportIfExists(bool reportIfExists);

        /**
         * @param fields 字段，必选字段
         */
        Builder &WithFields(const std::vector<std::shared_ptr<TableFieldProperty>> &fields);

        /**
         * @param embeddingMap 向量化字段map, key为标量字段名，value为对应向量字段名。表为向量表示，该字段必须存在
         */
        Builder &WithEmbeddingMap(const std::map<std::string, std::string> &embeddingMap);
        /**
         * @param timeOut 超时时间：，默认30s，非必选字段
         */
        Builder &WithTimeOut(uint32_t timeOut);
        std::shared_ptr<CreateTableOption> Build();

    private:
        class Impl;
        std::shared_ptr<Impl> m_impl;
    };

public:
    virtual const std::string &GetSchemaName() const = 0;
    virtual bool IsReportIfExists() const = 0;
    virtual const std::vector<std::shared_ptr<TableFieldProperty>> &GetFields() const = 0;
    virtual const std::map<std::string, std::string> &GetEmbeddingMap() const = 0;
    virtual uint32_t GetTimeOut() const = 0;
};
enum class VectorDistanceAlgorithm {
    IsSetDistance,
    L2,
    COSINE,
};
enum class VectorIndexAlgorithm {
    IsSetIndex,
    IVFFLAT,
    DISKANN
};

// --- 为了FilterOption兼容RAG SDK的接口 需要额外记录构建过程 ---
// 定义操作类型
enum class OpType {
    FieldName,
    Value,
    And,
    Or,
    NE,
    EQ,
    GT,
    LT,
    In
};

// 定义操作数据结构
struct Operation {
    OpType type;
    // 使用 variant 存储不同类型的数据
    std::variant<
        std::string,
        std::shared_ptr<TableValue>,
        std::vector<std::shared_ptr<TableValue>>
    > data;
};

/**
* 过滤条件需要严格按照逻辑顺序拼接
*/
class FilterOption {
public:
    virtual ~FilterOption();
    virtual std::string ToStr() = 0;

    // 获取操作序列的纯虚函数
    virtual const std::vector<Operation>& GetOperations() const = 0;

public:
    class Builder final {
    public:
        /**
         * 过滤条件需要严格按照逻辑顺序拼接
         */
        Builder();
        /**
         * @param fieldName 字段名称
         */
        Builder &WithFieldName(const std::string fieldName);
        /**
         * @param value 字段值, 字段值和字段名之间需要有条件运算符
         */
        Builder &WithValue(std::shared_ptr<TableValue> value);
        /**
         *  与条件
         */
        Builder &WithAnd();
        /**
         * or条件
         */
        Builder &WithOr();
        /**
         * 不等于
         */
        Builder &WithNE();
        /**
         * 等于
         */
        Builder &WithEQ();
        /**
         * 大于
         */
        Builder &WithGT();
        /**
         * 小于
         */
        Builder &WithLT();
        /**
         * 范围查询 in
         */
        Builder &WithIn(const std::vector<std::shared_ptr<TableValue>> &values);
        /**
         * 转 string
         */

        std::shared_ptr<FilterOption> Build();

    private:
        class FilterOptionImpl;
        std::shared_ptr<FilterOptionImpl> m_impl;
    };
};
enum class TableQueryType {
    IsSetTableQueryType,
    VECTOR,  // 向量
    TEXT,    // 文本
    SCALAR,  // 标量
    FUSION   // 融合
};

class QueryOption {
public:
    enum class QueryOrder { IsSetQueryOrder, ASC, DESC };
    enum class FusionMethod { IsSetFusionMethod, RRF, RERANK };

public:
    virtual ~QueryOption();

public:
    class Builder final {
    public:
        /**
         * @param queryType 查询类型
         */
        explicit Builder(TableQueryType queryType);
        // 基础参数
        /**
         * @param schemaName 查询数据表所属的schemaName，schema如果不指定则默认为public,非必选参数
         */
        Builder &WithBaseSchemaName(const std::string &schemaName);
        /**
         * @param filters 过滤条件，非必须参数 两套都支持
         */
        Builder &WithBaseFilters(std::shared_ptr<FilterOption> filters);
        /**
         * @param selectFields 查询字段，如果不设置，默认查询全部，如果查询数量：则字段设置为：COUNT，非必选参数
         */
        Builder &WithBaseSelectFields(const std::vector<std::string> &selectFields);
        /**
         * @param orderField 排序字段，与order配合，非必选参数
         */
        Builder &WithBaseOrderField(const std::string &orderField);

        /**
         * @param order 排序，与orderField配合，非必选参数
         */
        Builder &WithBaseOrder(QueryOrder order);
        /**
         * @param group 分组，非必选参数
         */
        Builder &WithBaseGroup(const std::string &group);

        /**
          * @param timeOut 超时时间 毫秒, 非必须参数
          */
        Builder &WithBaseTimeOut(uint32_t timeOut);
        // 标量
        /**
         * @param limit 标量查询，指定返回数据条数，非必选参数
         */
        Builder &WithScalarLimit(int limit);
        /**
         * @param offset 标量查询，指定从所查询的数据中从0开始偏移offset条数据，非必选参数
         */
        Builder &WithScalarOffset(int offset);
        // 向量
        /**
         * @param query 输入的用于与向量化字段进行向量相似性查询的字符串,该字符串输入时非向量化， sdk会调用embedding接口进行向量化,
         *      向量查询时必选其一 [WithVectorQuery/WithVectorValue] 优先取用[WithVectorValue]
         */
        Builder &WithVectorQuery(const std::string &query);
        /**
         * @param query 输入的用于与向量化字段进行向量相似性查询的字符串,该字符串输入非向量化，
         *      向量查询时必选其一 [WithVectorQuery/WithVectorValue] 优先取用[WithVectorValue]
         */
        Builder &WithVectorValue(const std::vector<float> &query);
        /**
         * @param filed 向量字段名 向量查询和混合检索时,必选参数
         */
        Builder &WithVectorField(const std::string &filed);
        /**
         * @param filed 向量索引名 ,必选参数
         */
        Builder &WithVectorIndex(const std::string &index);
        /**
         * @param disAlgo 向量距离计算方式，向量查询和混合检索时, 必选参数
         */
        Builder &WithVectorDistanceAlgorithm(VectorDistanceAlgorithm disAlgo);
        /**
         * @param topK 返回最相似的前k条数据，默认返回top10, 向量查询, 非必选参数
         */
        Builder &WithVectorTopK(int topK);
        /**
         * 设置返回向量距离,默认为false, 设置后为true，result 第一列为向量距离 向量查询非必选参数
         */
        Builder &WithVectorReturnDistance();
        /**
         * 设置过滤向量距离阈值，设置后过滤向量距离小于等于该阈值的结果
         * 默认为-1，当阈值传递为负值时 不做过滤处理
         * @param distance 向量距离阈值
         */
        Builder &WithVectorDistanceThreshold(float distance);

        // 文本
        /**
         * @param query 输入的用于文本相似性查询的字符串, 文本查询必选参数
         */
        Builder &WithTextQuery(const std::string &query);
        /**
         * @param filed 文本相似性查询字段名, 文本查询必选参数
         */
        Builder &WithTextField(const std::string &filed);
        /**
         * @param index bm25索引名 ,必选参数
         */
        Builder &WithTextIndex(const std::string &index);
        /**
         * @param topK 返回最相似的前k条数据， 默认为10, 文本查询非必选参数
         */
        Builder &WithTextTopK(int topK);
        /**
         * 设置返回BM25分数，默认false, 如果true，result 第一列为BM25分数, 文本查询非必选参数
         */
        Builder &WithTextReturnScore();
        /**
         * 设置过滤文本得分阈值，设置后过滤文本得分大于等于该阈值的结果
         * 默认为-1，当阈值传递为负值时 不做过滤处理
         * @param score 文本得分阈值
         */
        Builder &WithTextScoreThreshold(float score);
        // 混合搜索
        /**
                * @param query 输入的用于混合查询的字符串, 混合查询, 必选参数
                */
        Builder &WithFusionQuery(const std::string &query);
        /**
         * @param method 混合搜索结果排序方式，当前只支持 rrf, 默认rrf, 非必选参数
         */

        Builder &WithFusionMethod(FusionMethod fusionMethod);
        /**
         * @param topK 混合搜索top值，默认top 10，非必选参数
         */
        Builder &WithFusionTopK(int topK);
        /**
         * @param k 混合搜索结果排序rrf的基数值，默认60，非必选参数
         */
        Builder &WithFusionRRFK(int k);
        /**
         * @param params 混合检索排序算法参数
         */
        Builder &WithFusionParams(std::map<std::string, std::string> params);
        /**
         * @param field 重排序取前k条数据，默认top10
         */
        Builder &WithReRankField(std::string field);
        /**
         * @param topk 重排序字段
         */
        Builder &WithReRankTopK(int topk);
        /**
         * @param vocab 分词使用的词汇表，只有标量接口可以使用
         */
        Builder &WithVocab(std::vector<std::string> &vocab);

        /**
         * 分词过滤应用字段，只有标量接口可以使用
         *
         * @param segmentFieldName 字段名
         */
        Builder &WithWordSegmentField(const std::vector<std::string> &segmentFieldName);

        /**
         * 待分词语句，只有标量接口可以使用
         *
         * @param segmentQuery 待分词语句
         */
        Builder &WithSegmentQuery(const std::string &segmentQuery);

        std::shared_ptr<QueryOption> Build();

    private:
        class ScalarQueryOptionImpl;
        class VectorQueryOptionImpl;
        class TextQueryOptionImpl;
        class FusionQueryOptionImpl;
        std::shared_ptr<ScalarQueryOptionImpl> m_scalarImpl;
        std::shared_ptr<VectorQueryOptionImpl> m_vectorImpl;
        std::shared_ptr<TextQueryOptionImpl> m_textImpl;
        std::shared_ptr<FusionQueryOptionImpl> m_fusionImpl;
    };

public:
    virtual const std::string &GetSchemaName() const = 0;
    virtual const std::vector<std::string> &GetSelectFields() const = 0;
    virtual const std::shared_ptr<FilterOption> &GetFilters() const = 0;
    virtual const std::string &GetOrderField() const = 0;
    virtual QueryOrder GetOrder() const = 0;
    virtual const std::string &GetGroup() const = 0;
    virtual uint32_t GetTimeOut() const = 0;
    virtual int GetLimit() const = 0;
    virtual int GetOffset() const = 0;
    virtual std::vector<std::string> GetVocab() const = 0;
    virtual std::vector<std::string> GetWordSegmentField() const = 0;
    virtual const std::string &GetQuery() const = 0;
    virtual const std::vector<float> &GetVector() const = 0;
    virtual const std::string &GetField() const = 0;
    virtual const std::string &GetVectorField() const = 0;
    virtual const std::string &GetTextField() const = 0;
    virtual const std::string &GetVectorIndex() const = 0;
    virtual const std::string &GetTextIndex() const = 0;
    virtual VectorDistanceAlgorithm GetDisAlgo() const = 0;
    virtual int GetTopk() const = 0;
    virtual int GetFusionRRFK() const = 0;
    virtual bool IsReturnDistance() const = 0;
    virtual bool IsReturnScore() const = 0;
    virtual float GetDistanceThreshold() const = 0;
    virtual float GetScoreThreshold() const = 0;
    virtual std::map<std::string, std::string> GetFusionParams() const = 0;
    virtual const std::string &GetReRankField() const = 0;
    virtual int GetReRankTopK() const = 0;
    virtual FusionMethod GetFusionMethod() const = 0;
};

class CreateSchemaOption {
public:
    virtual uint32_t GetTimeOut() const = 0;
    class Builder {
    public:
        Builder();
        Builder &WithTimeOut(uint32_t timeOut);
        std::shared_ptr<CreateSchemaOption> Build();

    private:
        class CreateSchemaOptionImpl;
        std::shared_ptr<CreateSchemaOptionImpl> m_prop;
    };
};

class DropSchemaOption {
public:
    virtual uint32_t GetTimeOut() const = 0;
    class Builder {
    public:
        Builder();
        Builder &WithTimeOut(uint32_t timeOut);
        std::shared_ptr<DropSchemaOption> Build();

    private:
        class DropSchemaOptionImpl;
        std::shared_ptr<DropSchemaOptionImpl> m_prop;
    };
};

class AlterTableOption {
public:
    enum class OpType {
        DEFAULT,
        ADD,
        DEL,
    };
    class Builder {
    public:
        Builder(OpType Type, const std::string &fieldName);
        Builder &WithType(OpType Type);
        Builder &WithSchemaName(const std::string &schemaName);
        Builder &WithFieldName(const std::string &fieldName);
        Builder &WithFieldProperty(std::shared_ptr<TableFieldProperty> fieldProperty);
        Builder &WithTimeOut(uint32_t timeOut);
        std::shared_ptr<AlterTableOption> Build();

    private:
        class AlterTableOptionImpl;
        std::shared_ptr<AlterTableOptionImpl> m_prop;
    };
    virtual OpType GetType() const = 0;
    virtual const std::string &GetSchemaName() const = 0;
    virtual const std::string &GetFieldName() const = 0;
    virtual std::shared_ptr<TableFieldProperty> GetFieldProperty() const = 0;
    virtual uint32_t GetTimeOut() const = 0;
};

class DropTableOption {
public:
    class Builder {
    public:
        Builder();
        Builder &WithSchemaName(const std::string &schemaName);
        Builder &WithTimeOut(uint32_t timeOut);
        std::shared_ptr<DropTableOption> Build();

    private:
        class DropTableOptionImpl;
        std::shared_ptr<DropTableOptionImpl> m_prop;
    };

public:
    virtual const std::string &GetSchemaName() const = 0;
    virtual uint32_t GetTimeOut() const = 0;
};

class VectorIndexOption {
public:
    class Builder {
    public:
        Builder(VectorDistanceAlgorithm disAlgo, VectorIndexAlgorithm algo);
        std::shared_ptr<VectorIndexOption> Build();

    private:
        class VectorIndexOptionImpl;
        std::shared_ptr<VectorIndexOptionImpl> m_prop;
    };

public:
    virtual VectorDistanceAlgorithm GetDisAlgo() const = 0;
    virtual VectorIndexAlgorithm GetAlgo() const = 0;
};

class TextIndexOption {
public:
    virtual int GetNumParallels() const = 0;
    class Builder {
    public:
        Builder(int numParallels);
        Builder &WithNumParallels(int numParallels);
        std::shared_ptr<TextIndexOption> Build();

    private:
        class TextIndexOptionImpl;
        std::shared_ptr<TextIndexOptionImpl> m_prop;
    };
};
class CreateIndexOption {
public:
    enum class TableIndexType {
        TableIndexType,
        LITE_VECTOR,
        CLOUD_VECTOR,
        TEXT,
        SCALAR,
    };

public:
    class Builder {
    public:
        Builder(const std::vector<std::string> &fields, const std::string &indexName, TableIndexType indexType);
        Builder &WithSchemaName(const std::string &schemaName);
        Builder &WithVectorIndexOption(std::shared_ptr<VectorIndexOption> vectorIndexOption);
        Builder &WithTextIndexOption(std::shared_ptr<TextIndexOption> textIndexOption);
        Builder &WithTimeOut(uint32_t timeOut);
        Builder &WithLocal();
        std::shared_ptr<CreateIndexOption> Build();

    private:
        class CreateIndexOptionImpl;
        std::shared_ptr<CreateIndexOptionImpl> m_prop;
    };

public:
    virtual const std::string &GetSchemaName() const = 0;
    virtual const std::vector<std::string> &GetFields() const = 0;
    virtual const std::string &GetIndexName() const = 0;
    virtual TableIndexType GetIndexType() const = 0;
    virtual std::shared_ptr<VectorIndexOption> GetVectorIndexOption() const = 0;
    virtual std::shared_ptr<TextIndexOption> GetTextIndexOption() const = 0;
    virtual uint32_t GetTimeOut() const = 0;
    virtual bool GetLocal() const = 0;
};

class DropIndexOption {
public:
    virtual const std::string &GetSchemaName() const = 0;
    virtual uint32_t GetTimeOut() const = 0;
    class Builder {
    public:
        Builder();
        Builder &WithSchemaName(const std::string &schemaName);
        Builder &WithTimeOut(uint32_t timeOut);
        std::shared_ptr<DropIndexOption> Build();

    private:
        class DropIndexOptionImpl;
        std::shared_ptr<DropIndexOptionImpl> m_prop;
    };
};

class InsertOption {
public:
    class Builder {
    public:
        Builder();
        Builder &WithSchemaName(const std::string &schemaName);
        Builder &WithIsOverwrite(bool isOverwrite);
        Builder &WithOverwriteField(const std::string &overwriteField);
        Builder &WithEmbeddingMap(const std::map<std::string, std::string> &embeddingMap);
        Builder &WithTimeOut(uint32_t timeOut);
        std::shared_ptr<InsertOption> Build();

    private:
        class InsertOptionImpl;
        std::shared_ptr<InsertOptionImpl> m_prop;
    };

public:
    virtual const std::string &GetSchemaName() const = 0;
    virtual bool GetIsOverwrite() const = 0;
    virtual const std::string &GetOverwriteField() const = 0;
    virtual const std::map<std::string, std::string> &GetEmbeddingMap() const = 0;
    virtual uint32_t GetTimeOut() const = 0;
};

class UpdateOption {
public:
    class Builder {
    public:
        Builder();
        Builder &WithSchemaName(const std::string &schemaName);
        Builder &WithFilters(std::shared_ptr<FilterOption> filters);
        Builder &WithEmbeddingMap(const std::map<std::string, std::string> &embeddingMap);
        Builder &WithTimeOut(uint32_t timeOut);
        std::shared_ptr<UpdateOption> Build();

    private:
        class UpdateOptionImpl;
        std::shared_ptr<UpdateOptionImpl> m_prop;
    };

public:
    virtual const std::string &GetSchemaName() const = 0;
    virtual std::shared_ptr<FilterOption> GetFilters() const = 0;
    virtual const std::map<std::string, std::string> &GetEmbeddingMap() const = 0;
    virtual uint32_t GetTimeOut() const = 0;
};

class DeleteOption {
public:
    class Builder {
    public:
        Builder();
        Builder &WithSchemaName(const std::string &schemaName);
        Builder &WithFilters(std::shared_ptr<FilterOption> filters);
        Builder &WithTimeOut(uint32_t timeOut);
        std::shared_ptr<DeleteOption> Build();

    private:
        class DeleteOptionImpl;
        std::shared_ptr<DeleteOptionImpl> m_prop;
    };

public:
    virtual const std::string &GetSchemaName() const = 0;
    virtual std::shared_ptr<FilterOption> GetFilters() const = 0;
    virtual uint32_t GetTimeOut() const = 0;
};

class SqlExecOption {
public:
    virtual uint32_t GetTimeOut() const = 0;
    class Builder {
    public:
        Builder();
        Builder &WithTimeOut(uint32_t timeOut);
        std::shared_ptr<SqlExecOption> Build();

    private:
        class SqlExecOptionImpl;
        std::shared_ptr<SqlExecOptionImpl> m_prop;
    };
};

class SqlQueryOption {
public:
    virtual uint32_t GetTimeOut() const = 0;
    class Builder {
    public:
        Builder();
        Builder &WithTimeOut(uint32_t timeOut);
        std::shared_ptr<SqlQueryOption> Build();

    private:
        class SqlQueryOptionImpl;
        std::shared_ptr<SqlQueryOptionImpl> m_prop;
    };
};

}

#endif  // RAGCOMPSDK_EXTERN_TABLE_OPTION_H
```

```c++
//retriever/ragcompsdk_table_value.h
#ifndef RAGCOMPSDK_EXTERN_TABLE_VALUE_H
#define RAGCOMPSDK_EXTERN_TABLE_VALUE_H

#include <memory>
#include <string>
#include <vector>

namespace AgentFrm::Plugin {

class TableValue {
public:
    virtual std::string ToStr() = 0;
};

class IntegerValue final : public TableValue {
public:
    explicit IntegerValue(int value);
    std::string ToStr() override;
    int GetValue();

private:
    class IntegerValueImpl;
    std::shared_ptr<IntegerValueImpl> m_impl;
};

class VarCharValue final : public TableValue {
public:
    explicit VarCharValue(const std::string &value);
    std::string ToStr() override;
    std::string GetValue();

private:
    class VarCharValueImpl;
    std::shared_ptr<VarCharValueImpl> m_impl;
};

class TextValue final : public TableValue {
public:
    explicit TextValue(const std::string &value);
    std::string ToStr() override;
    std::string GetValue();

private:
    class TextValueImpl;
    std::shared_ptr<TextValueImpl> m_impl;
};
class FloatVectorValue final : public TableValue {
public:
    explicit FloatVectorValue(const std::vector<float> &value);
    std::string ToStr() override;
    std::vector<float> GetValue();

private:
    class FloatVectorValueImpl;
    std::shared_ptr<FloatVectorValueImpl> m_impl;
};
// UTC时间 (不包含时区, 相当于时区为0)
class TimestampValue final : public TableValue {
public:
    // 默认使用服务端数据库配置的当前时间.
    explicit TimestampValue();
    explicit TimestampValue(int year, int mon, int day, int hour = 0, int minu = 0, int sec = 0);
    std::string ToStr() override;
    int GetYear();
    int GetMon();
    int GetDay();
    int GetHour();
    int GetMinu();
    int GetSec();

private:
    class TimestampValueImpl;
    std::shared_ptr<TimestampValueImpl> m_impl;
};

// 带时区的时间
class TimestampTZValue final : public TableValue {
public:
    // 默认使用服务端数据库配置的当前时间.
    explicit TimestampTZValue();
    // 默认使用服务端数据库配置的时区
    explicit TimestampTZValue(int year, int mon, int day, int hour = 0, int minu = 0, int sec = 0);
    std::string ToStr() override;
    int GetYear();
    int GetMon();
    int GetDay();
    int GetHour();
    int GetMinu();
    int GetSec();

private:
    class TimestampTZValueImpl;
    std::shared_ptr<TimestampTZValueImpl> m_impl;
};

class BooleanValue final : public TableValue {
public:
    explicit BooleanValue(bool value);
    std::string ToStr() override;
    bool GetValue();

private:
    class BooleanValueImpl;
    std::shared_ptr<BooleanValueImpl> m_impl;
};

class ByteaValue final : public TableValue {
public:
    explicit ByteaValue(const std::vector<unsigned char> &bytes);
    std::string ToStr() override;
    std::vector<unsigned char> GetValue();

private:
    class ByteaValueImpl;
    std::shared_ptr<ByteaValueImpl> m_impl;
};

}

#endif // RAGCOMPSDK_EXTERN_TABLE_VALUE_H
```

## 3.3 ToolPlugin插件

对外发布的接口如下：其中ToolPlugin为接口类，其余为参数描述。

```c++
// tool_plugin.h
#ifndef MM_TOOL_PLUGIN_H
#define MM_TOOL_PLUGIN_H

#include <map>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <unordered_map>
#include "utils/class_util.h"
#include "plugin_manager.h" 

namespace AgentFrm::Plugin {

    // 错误码定义
    enum class ErrorCode : uint32_t {
        SUCCESS = 0,
        ERROR = 1,
        MM_AT_REQUEST_ERR = 1001,       // 请求AgentTool失败
        MM_AT_RESPONSE_ERR = 1002,      // AgentTool响应错误
        MM_AT_RESPONSE_BODY_ERR = 1003, // AgentTool响应体解析失败
    };

    typedef struct {
        /**
         * 工具名称
         */
        std::string name;
        /**
         * 工具描述
         */
        std::string description;
        /**
         * 工具的输入参数描述
         * 格式定义(MCP标准)
           {
                "description": "A JSON Schema object defining the expected parameters for the tool.",
                "properties": {
                    "properties": {
                        "type": "object"
                    },
                    "required": {
                        "items": {
                            "type": "string"
                        },
                        "type": "array"
                    },
                    "type": {
                        "const": "object",
                        "type": "string"
                    }
                },
                "required": [
                    "type"
                ],
                "type": "object"
            }
         *
         * 实际样例
         *
           {
            "type": "object",
            "required": [
                "interval",
                "count",
                "caller"
            ],
            "properties": {
                "interval": {
                "type": "number",
                "description": "Interval between notifications in seconds"
                },
                "count": {
                "type": "number",
                "description": "Number of notifications to send"
                },
                "caller": {
                "type": "string",
                "description": "Identifier of the caller to include in notifications"
                }
            }
            }
         *
        */
        std::string inputSchema;

        /**
         * 工具的输出参数描述
         * 格式定义(MCP标准)
         *
         * {
                "description": "An optional JSON Schema object defining the structure of the tool's output returned
                                in the structuredContent field of a CallToolResult.",
                "properties": {
                    "properties": {
                        "type": "object"
                    },
                    "required": {
                        "items": {
                            "type": "string"
                        },
                        "type": "array"
                    },
                    "type": {
                        "const": "object",
                        "type": "string"
                    }
                },
                "required": [
                    "type"
                ],
                "type": "object"
            }
         *
         * 实际样例
         *
            {
            "type": "object",
            "properties": {
                "temperature": {
                "type": "number",
                "description": "Temperature in Celsius"
                },
                "condition": {
                "type": "string",
                "description": "Weather condition"
                },
                "humidity": {
                "type": "number",
                "description": "Humidity percentage"
                },
                "city": {
                "type": "string",
                "description": "City name"
                }
            },
            "required": [
                "temperature",
                "condition",
                "humidity",
                "city"
            ]
            }
         *
         *
        */
        std::string outputSchema;
    } MMListToolResult;


    enum class MMCallToolResultType {
        /**
         * 文本类型
         */
        TEXT
    };

    /**
     * 文本类型内容
     */
    typedef struct {
        std::string text;
    } MMCallToolResultTextContent;

    /**
     * 调用响应结构体，后续用该结构体包含多个字段进行扩展
     */
    typedef struct {
        /**
         * 响应类型，当前仅支持文本
         */
        MMCallToolResultType type;
        /**
         * 不同类型使用的对象不同，当前仅文本，后续支持扩展
         */
        MMCallToolResultTextContent textContent;
    } MMCallToolContent;

    /**
     * 工具调用接口
     * content: 响应结果，当前仅支持文本
     */
    typedef struct {
        std::vector <MMCallToolContent> content;
    } MMCallToolResult;

    typedef uint32_t (*MMWriteLogFunc) (uint32_t level, const std::string& str);
    typedef uint32_t (*MMWriteLogFuncEx) (uint32_t level, const std::string& fileName, int lineNo, const std::string& str);

    /**
    * 日志钩子结构体
    */
    typedef struct tagLogReg {
        std::string modelId;  /**< 日志模块ID */
        uint32_t levelDebug;  /**< debug级别日志 */
        uint32_t levelInfo;  /**< info级别日志 */
        uint32_t levelWarn;  /**< warn级别日志 */
        uint32_t levelError;  /**< error级别日志 */
        uint32_t levelFatal; /**< fatal级别日志 */
        MMWriteLogFunc writeLog{nullptr}; /**< 写文本日志回调函数 */
        MMWriteLogFuncEx writeLogEx{nullptr}; /**< 写文本日志回调函数 */
    } MMToolIntfLogReg;


    class MMToolIntfInitParam {
    public:
        MMToolIntfInitParam() : m_logReg{} {}

        // 设置日志初始化参数
        void SetLogReg(const MMToolIntfLogReg& logReg) {
            m_logReg = logReg;
        }

        // 获取日志初始化参数
        const MMToolIntfLogReg& GetLogReg() const {
            return m_logReg;
        }

    private:
        MMToolIntfLogReg m_logReg;
    };

    extern "C" Plugin* CreatePlugin();
    extern "C" void DestroyPlugin(Plugin* ptr);

    class ToolPlugin : public Plugin, public CMFrm::Utils::SingletonBase  {
        friend Plugin* CreatePlugin();
    public:
        /**
         * 获取单例
         */
        static std::shared_ptr<ToolPlugin> GetInstance() {
            static std::shared_ptr<ToolPlugin> instance(new ToolPlugin());
            return instance;
        }

        /**
         * 插件类型
         *
         * @return 插件类型
         */
        std::string GetType() override;

        /**
         * 插件名称
         *
         * @return 插件名称
         */
        std::string GetName() override;

        /**
         * 插件标签，可以无标签，也可以有多个
         *
         * @return 插件名称
         */
        std::vector<std::string> GetTags() override;

        /**
         * 初始化接口
         */
        bool Init(MMToolIntfInitParam &param);

        /**
         * 工具查询接口
         * 返回值：执行结果，ErrorCode::SUCCESS成功，其他失败
         * 输出参数: tools，工具查询结果
         */
        uint32_t ListTools(std::vector <MMListToolResult> &tools);

        /**
         * 工具调用接口
         * 返回值：执行结果，ErrorCode::SUCCESS成功，其他失败
         * 输入参数：toolName，工具名称，样例tool1
         *          param，参数，JSON对象，包含自定义参数，自定义参数参见MMListToolResult inputSchema
         *          样例: {"interval": 1, "count": "true", "caller": "caller"}
         * 输出参数: output，结果，JSON数组，包含类型（文本）和自定义响应，自定义响应格式参见MMListToolResult outputSchema
         *           样例：[{"type": "text", "text": {"temperature": 1, "condition": "123", "humidity":1,
         *                 "city": "beijing"}}]
         */
        uint32_t CallTool(const std::string &toolName, const std::string &param, MMCallToolResult &result);

        ~ToolPlugin() override;

    private:
        // 私有构造函数
        ToolPlugin();

        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}  // namespace AgentFrm::Plugin

#endif  // MM_TOOL_PLUGIN_H

```


