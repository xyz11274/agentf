# 1. 概述

## 1.1 目的

描述AF格式Agent低码化描述规范，指导低码化开发

## 1.2 术语介绍

DSL：Domain‑Specific Language，这里特指使用AF格式，使用json文件的方式，描述Agent。
workflow：通过对插件、大语言模型等功能进行组合，从而实现复杂、稳定的业务流程编排。
节点（node）：提供独立功能的节点，为workflow提供原子能力。包含组件类型及组件实例。

# 2 DSL关系概述

Agent DSL、Workflow DSL、Node DSL均为json文件，其组合关系如下：

1）Agent DSL使用DSL方式，描述Agent信息
2）Workflow DSL描述Agent执行流程，其中包含和nodes和edges，其中nodes为节点信息描述，edges为边描述

# 3 开发流程

## 3.1 整体思路

1）按照THIRD IR规范，或者AF DSL方式，以json文件方式，描述低码化Agent
2）如果是THIRD IR规范开发的低码化Agent，则需要转换为AF DSL格式，由DSLAdapter模块完成
3）AgentFlow代码，解析AF DSL文件，根据文件描述逐步执行各节点，最后输出

## 3.2 按照af dsl规范开发Agent

备注：如果需要按照THIRD IR规范开发Agent，可以参考THIRD IR规范描述，此处不作额外说明
1）定义Node类型，Node中制定要执行的代码段，及输入、输出信息
2）定义edges结构体，其定义了Node间的DAG关系
3）将已定义的nodes和edges，写入到workflow中，补齐workflow其他字段
4）将workflow信息，填充到Agent中
5）将Agent交接给低码化引擎执行

# 4Agent DSL规范

## 4.1 Agent DSL字段描述

|字段|类型|是否必选|说明|
|-|-|-|-|
|name|String|必选|MMAgent名称，长度1-128，只能包含以下3种类型：1）字母（A-Z或a-z）；2)数字（0-9）；3)特殊字符：-_|
|version|String|必选|MMAgent版本号，格式为x.x.x，例如1.0.0|
|description|String|可选|agent描述信息|
|session_pool_size|uint|可选|预创建session池大小（>=1）,按同一份session模板实例化|
|max_workflows|int|必选|agent允许装载的workflow上限（>=1）|
|max_concurrent_workflows|int|必选|agent允许并发的workflow上限（>=1）|
|tasks|Task[]|必选|任务数组，Task定义见下文；当前要求包含1各contextupdater任务|
|sessions|Session[]|必选|同质化session模板定义，固定只配置1个session|

## 4.2 Session字段描述

|字段|类型|是否必选|说明|
|-|-|-|-|
|id|int|必选|从0开始连续编号|
|description|String|可选|session描述信息|
|task_count|int|必选|任务数量|
|tasks|Task[]|任务数组，Task定义见下文，长度和task_count保持一致|

## 4.3 Task字段描述

|字段|类型|是否必选|说明|
|-|-|-|-|
|id|int|必选|task唯一标识，长度1-128，只能包含以下3种类型：1）字母（A-Z或a-z）；2)数字（0-9）；3)特殊字符：-_|
|type|string|必选|如果在Agent DSL中，则固定为contextupdater；如果在session中，则可取值：dispatcher/coordinator/skill；|
|description|String|必选|描述信息|
|workflow_file|String|必选|任务绑定的workflow文件|
|skill|Skill|当type==skill时必选|skill描述信息|
|workflow_pool_size|int|必选|workflow多实例池大小（>=1，默认1）；runtime会按该值预创建workflow实例池|

* `workflow_pool_size` 与 `skill.instance_pool_size` 语义不同：
  * `workflow_pool_size` 描述 task 绑定 workflow 的实例池规模，并由 deployer / scheduler 在运行时建立 workflow 实例与 worker / task 的绑定；
  * `skill.instance_pool_size` 描述 skill task 自身应展开出的任务实例数。
 
* `dispatcher` / `coordinator` 是特殊 task：workflow 输出 `selected_skills` 后，agent 会继续把这些 skill 具象化为 runtime task。
* 每个 session 模板必须包含 1 个 `dispatcher`、1 个 `coordinator`，以及至少 1 个 `skill`

## 4.4 Skill字段描述

|字段|类型|是否必选|说明|
|-|-|-|-|
|name|string|必选|skill名称|
|config_dir|string|必选|skill配置文件路径|
|instance_pool_size|int|必选|实例资源池个数|

# 5Workflow DSL规范

## 5.1 元素组成

|字段|类型|是否必选|说明|
|-|:-:|-|-|
|id|String|必选|Workflow的唯一标识，长度1-128，只能包含以下3种类型：1）字母（A-Z或a-z）；2)数字（0-9）；3)特殊字符：-_
|version|String|必选|当规范不能兼容以前规范时需要刷新该字段版本号，例如1.0
|name|String|必选|Workflow名称，长度1-256，可用于大模型输入，提升准确度
|description|String|可选|workflow的描述信息
|nodes|[]Node|必选|组成workflow流程的组件列表，不能为空
|edges|[]Edge|必选|描述组件之间的依赖关系

## 5.2 Node描述

Node实例由以下元素组成：

|字段|类型|是否必选|说明
|-|-|-|-
|id|String|必选|节点的唯一id，只能包含以下3种类型：1）字母（A-Z或a-z）；2)数字（0-9）；3)特殊字符：-_ 。
|name|String|必选|节点名称，只能包含以下3种类型：1）字母（A-Z或a-z）；2）数字（0-9）；3）特殊字符：-_ 。
|type|String|必选|组件类型版本，命名只能包含以下3种类型：1）字母（A-Z或a-z）；2)数字（0-9）。
|execution_mode|String|必选|执行模式，例如shell_script|
|command|String|必选|模型执行命令|
|input_params|map[String, Value]|可选|节点输入参数列表|
|output_params|map[String, Value]]|可选|节点输出参数列表|

### 5.2.1 值描述 - Value

|字段|类型|是否必选|说明
|-|-|-|-
|id|String|必选|值的id名称，长度1-128。只能包含如下3种类型：1)字母（A-Z或a-z）；2)数字（0-9）；3)特殊字符：-_
|description|String|可选|值的描述
|type|ValueTypeEnum|必选|值的类型
|constraint|Map|可选|对于值的扩展约束信息，比如该值的范围字段

### 5.2.2 组件值的类型范围 - ValueTypeEnum

ValueTypeEnum取值范围如下：

|类型|说明
|-|-
|String|基本类型:字符串
|Int|整数
|Float|浮点数
|Boolean|Bool值
|List|列表，其中T可以时ValueTypeEnum的一种，或填入List、List表示任意类型的列表。
|Map|字段，其中T可以是ValueTypeEnum的一种，或者填入T表示任意类型的字典。Map与Map等价。

### 5.3 组件关系 - Edge

表示nodes中组件的连接方式，具体字段如下：

|字段|类型|是否必选|说明
|-|-|-|-
|id|String|必选|边的唯一ID，只能包含如下3种类型：1）字母（A-Z或a-z）；2）数字（0-9）；3）特殊字符：-_ 。
|source|String|必选|源节点id
|target|String|必选|目标节点id
|condition_expression|String|可选|判断条件


