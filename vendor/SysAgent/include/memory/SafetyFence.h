// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_SAFETYFENCE_H
#define SYSAGENT_SAFETYFENCE_H
#include "json/json.h"
#include <string>
#include <vector>
#include "graphManager/GraphUtils.h"

namespace SYSAGENT {

class SafetyFence {
public:
    std::string id;
    std::string query;
    std::string content;
    std::string sceneType;

    SafetyFence(std::string query, std::string content, std::string sceneType)
    {
        this->query = query;
        this->content = content;
        this->sceneType = sceneType;
    }

    SafetyFence(std::string id, std::string query, std::string content, std::string sceneType)
    {
        this->id = id;
        this->query = query;
        this->content = content;
        this->sceneType = sceneType;
    }

    /**
     * 根据 instruction 做相似度查询，按字段返回信息
     * @param instruction
     * @param fields
     * @return
     */
    static std::vector<std::string> SimilaritySearch(std::string instruction, std::vector<std::string> fields);

    /**
     * 解析content内容
     * @param data
     * @return
     */
    static std::string parseRejectContent(const std::string& data);

    /*
     * 解析融合视频的无参指令意图
     */
    static std::string parseIntentName(const std::string& data);
};

bool LoadLocalSafetyFence();
void Insert(const std::vector<SafetyFence>& sfs);

} // namespace SYSAGENT
#endif // SYSAGENT_SAFETYFENCE_H