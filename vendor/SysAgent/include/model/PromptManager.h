// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_PROMPT_MANAGER_H
#define SYSAGENT_PROMPT_MANAGER_H

#include <cstdint>
#include <map>
#include <string>
#include "mm_prompttmplt.h"
#include "config/Config.h"
#ifdef Expand
#undef Expand
#endif
#include "rapidjson/document.h"
#include "rapidjson/rapidjson.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "registerManage/AgentManager.h"

/*
1、场景分类使用固定的prompt
2、意图裁决根据不同场景，提取对应场景所有的意图，作为参数渲染prompt模板(意图组装成json)
   格式: [{'intent': 'INTENT_INC_VOLUME', 'description': '音量调大'}, {'intent': 'INTENT_DEC_VOLUME', 'description': '音量调小'}]
3、参数提取根据不同意图，提取对应意图所有的参数，作为参数渲染prompt模板(参数组装成json)
   格式: [{'name': 'audioType', 'type': 'string', 'description': '0：系统声音；1音乐声音(背景音)；2：教练声音（课程中教练的口播）；默认
   值为0', 'required': '0'}]
4、参数裁决根据不同意图，提取对应意图所有的参数，作为参数渲染prompt模板(每个参数一行)
   格式: audioType:int类型，0：系统声音；1音乐声音(背景音)；2：教练声音（课程中教练的口播）；默认值为0
*/
class PromptManager {
public:
    static PromptManager& GetInstance()
    {
        static PromptManager instance = PromptManager();
        return instance;
    }

    // 加载prompts
    void Init()
    {
        for (const auto& prompt : ConfigManager::getInstance().getPromptInfoList()) {
            // 如果是intent_adjudicatec，提前进行渲染
            if (prompt.name == "intent_parameter_decision") {
                const auto& intents = ConfigManager::getInstance().getIntents();
                rapidjson::Document doc;
                doc.SetObject();
                rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();
                for (const auto& [intent, intentInfo] : intents) {
                    if (!doc.HasMember(intentInfo.categoryId.c_str())) {
                        // 第一次添加array
                        auto array = rapidjson::Value(rapidjson::kArrayType);
                        rapidjson::Value categoryId;
                        categoryId.SetString(intentInfo.categoryId.c_str(), allocator);
                        doc.AddMember(categoryId, array, allocator);
                    }
                    rapidjson::Value& arr = doc[intentInfo.categoryId.c_str()];
                    rapidjson::Value childObj(rapidjson::kObjectType);
                    rapidjson::Value intentValue;
                    intentValue.SetString(intent.c_str(), allocator);
                    childObj.AddMember("intent", intentValue, allocator);
                    rapidjson::Value description;
                    description.SetString(intentInfo.description.c_str(), allocator);
                    childObj.AddMember("description", description, allocator);
                    rapidjson::Value args;
                    args.SetString(ConfigManager::getInstance().getIntentToParamsString()[intentInfo.intent].c_str(),
                                   allocator);
                    childObj.AddMember("args", args, allocator);
                    arr.PushBack(childObj, allocator);
                }

                for (auto itr = doc.MemberBegin(); itr != doc.MemberEnd(); ++itr) {
                    std::string promptTemplate = prompt.promptTemplate;
                    auto tmpl = mmsdk::MMPromptTemplate({.name = prompt.name, .owner = "SysAgent"});
                    tmpl.SetContent(promptTemplate);
                    std::string category = itr->name.GetString();
                    rapidjson::StringBuffer buffer;
                    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
                    itr->value.Accept(writer);
                    auto paramsStr = buffer.GetString();
                    LOG_INFO_EX("IntentAdjudicate Prompt: %s, paramsStr: [%s]",
                                std::string("intent_adjudicatec_" + category).c_str(), paramsStr);
                    tmpl.RenderPrompt({{"intents_desc", paramsStr}});
                    prompts_.emplace("intent_adjudicatec_" + category, tmpl);
                }
            }
            std::string promptTemplate = prompt.promptTemplate;
            auto tmpl = mmsdk::MMPromptTemplate({.name = prompt.name, .owner = "SysAgent"});
            tmpl.SetContent(promptTemplate);
            LOG_INFO_EX("add prompt: [%s]", prompt.name.c_str());
            prompts_.emplace(prompt.name, tmpl);
        }
        LOG_INFO_EX("add prompt count: [%d]", prompts_.size());
    }

    const mmsdk::MMPromptTemplate GetPormptTmpl(const std::string& name)
    {
        LOG_INFO_EX("get prompt [%s], all: %d", name.c_str(), prompts_.size());
        auto it = prompts_.find(name);
        if (it == prompts_.end()) {
            LOG_INFO_EX("not found prompt [%s]", name.c_str());
            return mmsdk::MMPromptTemplate({.name = "", .owner = "SysAgent"});
        }
        return it->second;
    }

    const mmsdk::MMPromptTemplate GetIntentAdjudicatecPrompt(const std::string& categoryId)
    {
        LOG_INFO_EX("get IntentAdjudicatec prompt [%s]", std::string("intent_adjudicatec_" + categoryId).c_str());
        return prompts_.at("intent_adjudicatec_" + categoryId);
    }

private:
    std::map<std::string, mmsdk::MMPromptTemplate> prompts_;
};

#endif  // SYSAGENT_PROMPT_MANAGER_H