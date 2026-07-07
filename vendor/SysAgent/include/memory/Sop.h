// Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.

#ifndef SYSAGENT_SOP_H
#define SYSAGENT_SOP_H

#include <string>
#include <vector>
#include <map>
#include "json/json.h"

// SOP基类
class SOPBase {
public:
    std::string date;
    std::string id;
    std::string task;
    std::string description;
    std::vector<std::string> apps;
    std::string content;

    virtual ~SOPBase() = default;
public:
    SOPBase() {}
};


// Sop 类，继承自 Base
class SOP : public SOPBase {
public:
    std::string instruction;

    ~SOP() override = default;
public:
    SOP() {}

    /**
     * 构造函数
     * @param task
     * @param description
     * @param apps
     * @param instructions
     * @param content
     */
    SOP(const std::string d, const std::string i, const std::string t,
        const std::string desc, const std::vector<std::string> a,
        const std::string c, const std::string instr)
            : instruction(instr) {
        date = d;
        id = i;
        task = t;
        description = desc;
        apps = a;
        content = c;
    }

    /**
     * 根据 instruction 做相似度查询，按字段返回信息
     * @param instruction
     * @param fields
     * @return
     */
    static std::vector<std::string> SimilaritySearch(std::string instruction, std::vector<std::string> fields);
    static std::vector<std::string> SimilaritySearchTest(std::string instruction, std::vector<std::string> fields);
};


// SOPSet类，继承自SOPBase
class SOPSet : public SOPBase {
public:
    std::vector<std::string> instructions;
    static std::vector<SOP> sopList;

    ~SOPSet() override = default;
public:
    SOPSet() {}

    /**
     * 构造函数
     * @param task
     * @param description
     * @param apps
     * @param instructions
     * @param content
     */
    SOPSet(const std::string d, const std::string i, const std::string t,
           const std::string desc, const std::vector<std::string> a,
           const std::string c, const std::vector<std::string> instr)
            : instructions(instr) {
        date = d;
        id = i;
        task = t;
        description = desc;
        apps = a;
        content = c;
    }

    /**
     * 从本地加载SOP信息
     * @return
     */
    static bool LoadLocalSOP();

    /**
     * json 转为 SOPSet 对象
     * @param jsonData
     */
    void FromJson(const Json::Value& jsonData);

    /**
     * SOPSet 插入到数据库
     */
    void Insert();
};

#endif // SYSAGENT_SOP_H


