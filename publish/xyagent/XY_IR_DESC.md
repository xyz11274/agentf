# 1. 概述

## 1.1 目的

描述xy ir开发规范，指导低码化Agent开发

## 1.2 范围

包含ir规范、ir示例

## 1.3 术语

**IR**（Intermediate Representation），即中间表示的概念。在XY平台中，IR用来描述Agent、workflow开发态以及运行态的相关信息。
**Agent**：是一种能够自主理解、规划决策、执行复杂任务的智能体。workflow是实现Agent的一种方式。
**workflow**：通过对插件、大语言模型等功能进行组合，从而实现复杂、稳定的业务流程编排。
**组件（component）**：提供独立功能的节点，为workflow提供原子能力。包含组件类型及组件实例。
**组件类型（Component Type）**：组件类型是指一种独立可重用的节点，用于完成特定的功能。
**组件实例（Component Instance）**：组件实例是实例化后的组件类型，是workflow开发、执行的基本单元，用于描述workflow中一个流程节点的输入、输出、配置项等信息，以及组件间值的引用关系。
**组件类型规范（Component Type Schema）**：定义组件类型的数据结构及成员，包含输入、输出、配置项、类型等约束。
**组件实例规范（Component Instance Schema）**：定义组件实例的数据结构及成员，包含输入、输出、配置项、类型等约束。

# 2. IR关系概述

Agent IR、Workflow IR、Component IR、Component Type IR均为json文件，其组合关系如下：
1）Agent IR中包含Workerflow IR，Workflow通过有向图方式，描述了Agent的执行流程
2）Workflow IR描述Agent执行流程，其中包含和Component和Connection，其中Component表示节点，Connection表示边
3）Component中引用了Component Type，其中Component Type中定义了代码模块，在图解析执行到对应Component时，会调用相关模块函数执行
4）一个Component Type，可以对应多个Component，但是Component只能对应一个Component Type

# 3. IR开发流程

1. 定义组件类型：根据需要实现的功能，识别需要增加的组件类型。
2. 实现组件类型中的代码，并编译为代码包。
3. 实现workflow IR：实现组件实例的编排，组件实例遵从组件实例schema规范要求。
4. 实现Agent IR：Agent IR以json文件方式描述低码化Agent，其中包含了workflow工作流程。

# 4. Component Type

## 4.1 Component Type字段组成

ComponentType定义了组建类型规范，其中包含如下字段：

|字段|类型|是否必选|说明
|-|-|-|-
|schemaVersion|String|必选|IR规范版本，当规范不能兼容以前规范时需要刷新此字段。
|type|String|必选|组件类型
|version|String|必选|组件版本
|componentCatalog|String|可选|组件类型分类。分为baseComponent、compositeComponent两类。默认为baseComponent。baseComponent是编排中最小粒度的基础组件，compositeComponent是引用了workflow的组合组件。
|description|String|可选|组件的描述信息
|classInfo|Map|必选|描述组件的实现类型信息
|inputs|[]Value|可选|组件输入字段列表，可以为空
|outputs|[]Value|可选|组件输出字段列表，可以为空
|configs|[]Value|可选|组件配置字段，可以为空

Component Type为Component类型，可以单独定义json文件，被Component引用，示例如下：

## 4.2 组件实现信息 - classInfo

|字段|类型|是否必选|说明
|-|-|-|-
|programLanguage|String|必选|支持c++、go、java、python语言，统一为小写
|modulePath|String|可选|门模块路径，当programLanguage为java时，该字段为空，当programLanguage为go、python时，该字段不能为空。
|className|String|必选|类名，当programLangurage为java时，包含packageName、className
|filePath|String|必选|类文件路径，支持Linux风格路径，以斜杠分隔的绝对或相对路径

## 4.3 值描述 - Value

|字段|类型|是否必选|说明
|-|-|-|-
|id|String|必选|值的id名称，长度1-128。只能包含如下3种类型：1)字母（A-Z或a-z）；2)数字（0-9）；3)特殊字符：-_
|description|String|可选|值的描述
|type|ValueTypeEnum|必选|值的类型
|schema|List/Map|可选|对于复杂数据结构（Map、List）内部数据格式表示。当值的类型为Map时，schema类型为List，当值的类型为List时，schema类型为Map。
|constraint|Map|可选|对于值的扩展约束信息，比如该值的范围字段：“constraint”：{“max”：100}，需要在Component实现中增加检验

在同一个component中，configs中的值id不能重复；inputs中的值id不能重复；outputs中的值id不能重复。

## 4.4 组件值的类型范围 - ValueTypeEnum

|类型|说明
|-|-
|String|基本类型:字符串
|Int|整数
|Float|浮点数
|Boolean|Bool值
|List|列表，其中T可以时ValueTypeEnum的一种，或填入List、List表示任意类型的列表。
|Map|字典，其中T可以是ValueTypeEnum的一种，或者填入T表示任意类型的字典。Map与Map等价。

## 4.5 组合组件类型

组合组件CompositeComponent：将workflow按照引用方式封装成组件后，按组件方式参与workflow编排，实现workflow嵌套引用。

### 4.5.1 Planner组件

### 4.5.2 工作流组件

工作流组件实现工作流按组件形式在工作流中编排使用，从而实现工作流嵌套编排。意味着可以在一个主工作流中调用其他子工作流，以实现更加复杂的流程组装。或者将一个复杂任务分解成多个子工作流，在设计、拼装、引用时更加灵活和高效。

## 4.6 流程控制组件类型

### 4.6.1 分支组件switchCase
switchCase是用于分支选择的组件。

|  字段      |  类型           |  是否必选  |  说明
| ------------ | ----- | --| -|
|  id        |  String         |  必选      |  分支组件的id,长度1-128，只能包含以下3种类型： l 字母（A-Z或a-z） l 数字（0-9） l 特殊字符：-\_  |
|  name      |  String         |  可选      |  组件名称                                                                                  
|  type      |  String         |  必选      |  条件分支类型,当前支持的条件分支类型为switchCase    
|  branches  |  []Branch       |  必选      |  描述分支信息，不能为空                             
|  configs   |  Map  |  可选      |  配置信息

Branch中元素：

|  字段            |  类型    |  是否必选  |  说明
| ---| --- | --- | --
|  boolExpression  |  String  |  必选      |  Bool条件表达式，返回值必须为Boolean
|  branchId        |  String  |  必选      |  分支id,长度1-128，只能包含以下3种类型： l 字母（A-Z或a-z） l 数字（0-9） l 特殊字符：-\_  

"default"表示当各个分支表达式不满足条件时，则执行"default"分支。如果用户需要有"default"分支，需要在分支中显示说明。当id为default时，可以不写boolExpression。

•       分支条件采用表达式，基于表达式返回Bool值来确认分支命中情况。

•       多分支命中情况下，优先采用最前面命中的分支。

### 4.6.2 分支组件支持的bool表达式

Bool表达式用于在分支组件中进行条件分支的判断。

bool表达式支持的关键字：&&, ||, ==, !=

|  类型            |  描述                                     |  示例
| -- | -- | --
|  &&, \|\|        |  与、或连接，连接多个bool表达式                                                                                                |  (\${node\_start.query} == "testQuery") && ("query" == \${node\_start.hcy})                                                                                |
|  ==, !=          |  值比较，两个值是相同还是不同的判断，支持String、Boolean、Int、Float                                                           |  \${node\_start.query.subStr} == "testQuery" \${node\_start.query} != "testQuery"                                                                          |
|  >=, >, <=, <    |  大小判断，支持Int、Float                                                                                                      |  \${node\_start.age} >= 1                                                                        
|  is\_empty       |  判断是否为空。 对于Int、Float、Boolean类型，表示值是否存在的判断，对于String类型，表示值是否存在或值为空字符串的判断          |  is\_empty(\${node\_start.age}) is\_empty(\${node\_start.quey})                                                                                           
|  is\_not\_empty  |  判断是否不为空。 对于Int、Float、Boolean类型，表示值是否不存在的判断，对于String类型，表示值是否否存在或值不为空字符串的判断  |  is\_not\_empty(\${node\_start.age}) is\_not\_empty (\${node\_start.quey})                                                                                 |
|  length          |  字符串长度                                                 |  length(\${node\_start.query}) >= 1                                                       
|  in，not\_in     |  判断字符串是否包含子串                                                                                                        |  \${node\_start.query} in "test"，表示query变量值是“test”的子串时为true \${node\_start.query} not\_in "test"，表示query变量值不是“test”的子串时为true  |

约束与说明：

1. 参数校验需要上层应用执行，保证表达式合法（比如左右类型一致），否则结果可能会不符合预期。
2. 浮点数根据IEEE 754标准，比较时存在精度限制，精确到6位有效数字时会保证精准，超过可能会不符合预期，需要上层应用做限制，保证是6位有效数字。
   
   例如：
   float a = 12345.6f;  float b = 1.23456f // 6 位有效数字，保证精确
   float c = 12345.678f; // 第8位数字8可能丢失精度

3. Integer取值范围如下：-2147483648～2147483647


# 5. Agent IR规范

|字段|类型|是否必选|说明
|-|-|-|-
|schemaVersion|String|必选|当规范不能兼容以前规范时需要刷新该字段版本号，当前版本号0.6
|agentID|String|必选|Agent IR唯一标识，长度1-128，只能包含以下3种类型：1）字母（A-Z或a-z）；2)数字（0-9）；3)特殊字符：-_
|agentVersion|String|必选|AgentIR版本
|description|String|可选|AgentIR的描述信息
|workflow|WorkflowIR|必选|包含workflow IR信息

# 6. Workflow IR规范

## 6.1 元素组成

|字段|类型|是否必选|说明
|-|-|-|-
|schemaVersion|String|必选|当规范不能兼容以前规范时需要刷新该字段版本号，当前版本0.6
|workflowId|String|必选|Workflow的唯一标识，长度1-128，只能包含以下3种类型：1）字母（A-Z或a-z）；2)数字（0-9）；3)特殊字符：-_
|workflowName|String|必选|Workflow名称，长度1-256，可用于大模型输入，提升准确度
|workflowVersion|String|必选|Workflow版本，只能包含以下3种类型：1）字母（A-Z或a-z）；2)数字（0-9）；3)特殊字符：-_
|description|String|可选|workflow的描述信息
|config|Map|可选|描述workflow的全局配置
|components|[]Component|必选|组成workflow流程的组件列表，不能为空
|connections|[]Connection|必选|描述组件之间的依赖关系，不能为空

## 6.2 Component实例

Component实例由以下元素组成：

|字段|类型|是否必选|说明
|-|-|-|-
|id|String|必选|组件实例的唯一id，只能包含以下3种类型：1）字母（A-Z或a-z）；2)数字（0-9）；3)特殊字符：-_ 。组件id不能使用保留关键词：start、end、input、output、result、_finish、_sink
|type|String|必选|组件类型，区分不同类型的组件，对应Component Type中的type字段
|typeVersion|String|必选|组件类型版本，命名只能包含以下3种类型：1）字母（A-Z或a-z）；2)数字（0-9）
|name|String|可选|组件实例名称
|inputs|Map|可选|组件实例输入字段列表
|outputs|Map|可选|组件实例输出字段列表
|configs|Map|可选|组件实例可配置字段

## 6.3 组件关系 - Connection

描述components中组件的连接方式，具体字段如下：

|字段|类型|是否必选|说明
|-|-|-|-
|source|ConnectionInfo|必选|源组件
|target|ConnectionInfo|必选|目标组件
|ConnectionInfo中字段说明
|字段|类型|是否必选|说明
|componentId|String|必选|组件id
|branchId|String|可选|分支id，branchId只能在source节点中，且属于分支节点

注意：source和target不能配置为相同的id
示例：

```json
{
  "connections": [
    {
      "source": {
        "componentId": "Start12345678"
      },
      "target": {
        "componentId": "evaluationCondition"
      }
    },
    {
      "source": {
        "componentId": "evaluationCondition",
        "branchId": "refuse"
      },
      "target": {
        "componentId": "sendRefuse"
      }
    },
    {
      "source": {
        "componentId": "evaluationCondition",
        "branchId": "clarify"
      },
      "target": {
        "componentId": "sendClarify"
      }
    },
    {
      "source": {
        "componentId": "evaluationCondition",
        "branchId": "summary"
      },
      "target": {
        "componentId": "summary"
      }
    }
  ]
}
```


