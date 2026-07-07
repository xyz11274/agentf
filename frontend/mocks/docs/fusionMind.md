# FusionMind 接口

## 1.OpenAI规范推理任务接口

URL: POST
/infermind/llm/v1/chat/openai/completions

```
Header:
  requestId:111
```

其中requestId为一个32位的无符号整数，111为示例

除此之外， 这个接口应该就是标准的 openai 规范的接口


## 2.推理任务接口

URL: POST
/infermind/llm/v1/chat/completions

```
Header:
    Content-Type: application/json
```

这个推理接口应该也支持 tools, 但是和openai 规范的格式稍有不同


# Sysagent的 问题

vendor/SysAgent/src/sys_agent/IMInferServiceClient.cpp

这个实现的问题，实现的不全