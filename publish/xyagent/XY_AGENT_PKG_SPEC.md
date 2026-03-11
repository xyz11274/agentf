# 1. 概述

## 1.1 目的

## 1.2 范围

## 1.3 术语

# 2. Agent包概述

# 3. Agent包开发流程

# 4. Agent包格式规范

Agent包格式如下，以ShoppingAgent为示例说明：

````
MathAgent_0.0.1/
├ packages    # 包目录
│├ MathAgent_0.0.1.tar.gz
│├ MathAgent_0.0.1_description.json
│├ MathAgent_0.0.1_sign.tar.gz
├ package.json   # 包描述文件
├ crldata.crl      #  校验文件
├ vercfg.xml      #  校验文件
└ vercfg.xml.cms    #  校验文件
````

package.json为包描述文件，内容如下

```json
{
	"type": "agent",
	"version": "1.0",
	"packageList": [
		"config": "MathAgent_description.json",
		"package": "MathAgent_x.x.x.tar.gz",
		"sign": "MathAgent_sign.tar.gz"
	]
}
```

其中MathAgent_XY/FM_x.x.x.tar.gz为Agent包，支持xy格式与fm格式，其中xy格式定义见4.1，fm格式见4.2。

其中MathAgent_sign.tar.gz为签名文件，对config和package文件一起进行签名MathAgent_description.json为配置文件，内容如下：

```json
{
	"name": "MathAgent",
	"type": "agent",
	"description": "math agent package",
	"version": "1.0",
	"namespace": "ns01",
	"agent": {
		"configs": [
			{
				"name": "AgentWorkerNum",
				"value": 2
			}
		]
	}
}
```

## 4.1 xy格式Agent包

### 4.1.1 xy格式Agent包结构

MathAgent_XY_x.x.x.tar.gz为FM格式的低码化Agent包，其解压后为MathAgent_XY_x.x.x，内容如下：

````
MathAgent_XY_x.x.x   # 由MathAgent_XY_x.x.x.tar.gz解压而来
├ MathAgentIR.json    #  xy格式描述的低码化MathAgent 
└ MathComponent_x.x.x.tar.gz   # xy格式的节点代码````

其中MathComponent_x.x.x.tar.gz为节点实现，MathComponent_x.x.x对应xy ir中的节点类型字段。节点支持C++、Python，其中C++的节点实现见4.1.2章节
````

### 4.1.2 C++ Component结构描述

如Agent包中描述，其中MathComponent_x.x.x.tar.gz为C++ Node，其xy格式的定义如下：

```
MathComponent_x.x.x  # 为MathComponent_x.x.x.tar.gz解压后的目录
├ MathComponentTypeIR.json  # MathComponent Type的描述文件，具体格式参考：《xyir规范》第4章。
├ MathComponent_x.x.x.so
└ 其他依赖的so文件
```

其中MathComponent_x.x.x.so为C++ so文件，导出了如下接口。

### 4.1.3 C++ 节点so接口定义

C++ 节点 so导出的接口如下。执行过程中，低码化引擎并加载此so，通过其导出的Create接口，获取到NodeType对象指针，然后通过调用NodeType对象的HandleRequest方法，执行具体的实现函数，实现Node的执行。

```c++
// Node导出接口定义如下
class NodeType {
    public:
        virtual ErrorCode HandleRequest(const std::shared_ptr<Value> &input, std::shared_ptr<Value> &output, const Context &context) = 0;
    };

extern "C" NodeType* Create(std::string &componentType);
extern "C" void Destroy(NodeType *p);
```

其中Value为IR支持的基本数据类型，定义如下：

```c++
typedef
typedef std::map<std::string, std<shared_ptr<Value>>> ValueMap;

enum class ValueTypeEnum {
      STRING,
      INT,
      FLOAT,
      BOOLEAN,
      VALUELIST,
      VALUEMAP,
      BUTT
};

struct Value {
      std::any value;
      ValueTypeEnum valueType;
};
```

其中HandleRequest中的Context，为引擎传给Node的上下文信息，具体定义如下，接口中PluginInterface、pluginName为插件信息，Node可以直接调用三方插件提供的能力，具体参考《低码化引擎插件接口》中的描述。

```c++
class Context {
	shared_ptr<PluginInterface> getPluginInterface(const std::string pluginName);
	// ...
};
```

## 4.2 fm格式Agent包

### 4.2.1 fm格式Agent包结构

MathAgent_FM_x.x.x.tar.gz为FM格式的低码化Agent包，其解压后为MathAgent_FM_x.x.x，内容如下：

````
MathAgent_FM_x.x.x   # 由MathAgent_FM_x.x.x.tar.gz解压而来
├ MathAgentIR.json    #  fm格式描述的低码化MathAgent 
└ AddNode_x.x.x.tar.gz   # fm格式的节点代码````

其中MathNode_x.x.x.tar.gz为节点实现，MathNode_x.x.x对应fm ir中的节点类型字段。节点支持C++、Python，其中C++的节点实现见4.2.1章节
````

### 4.2.2 C++ Node结构描述

如Agent包中描述，其中MathNode_x.x.x.tar.gz为C++ Node，其fm格式的定义如下：

```
MathNode_x.x.x  # 为MathNode_x.x.x.tar.gz解压后的目录
├ MathNode_x.x.x.so
└ 其他依赖的so文件
```

其中MathNode_x.x.x.so为C++ so文件，导出了如下接口。

### 4.2.3 C++ Node so接口定义

C++节点so的接口定义，同xy格式节点定义，参考4.1.3章节

### 4.2.2 Python Node结构描述


