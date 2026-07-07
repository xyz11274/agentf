/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 */

#include "sys_agent/UDSInterface.h"
#include "rapidjson/rapidjson.h"
#include "rapidjson/document.h"
#include "rapidjson/writer.h"

namespace SysAgent::UDS {
    std::string UDSResponse::ToJson() const
    {
        rapidjson::Document doc;
        doc.SetObject();  // 创建根对象
        rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();

        // 1. 字符串字段（直接拷贝）
        doc.AddMember("request_id", rapidjson::Value(request_id.c_str(), allocator), allocator);
        doc.AddMember("status", rapidjson::Value(status.c_str(), allocator), allocator);
        doc.AddMember("error_message", rapidjson::Value(error_message.c_str(), allocator), allocator);
        doc.AddMember("data", rapidjson::Value(data_json.c_str(), allocator), allocator);

        // 2. 枚举错误码：转为整数
        doc.AddMember("error_code", static_cast<int>(error_code), allocator);
        doc.AddMember("timestamp", static_cast<uint64_t>(timestamp), allocator);

        // 4. 生成JSON字符串
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);

        return buffer.GetString();
    }

    std::string UDSEvent::ToJson() const
    {
        rapidjson::Document doc;
        doc.SetObject();
        rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();

        // 字符串字段
        doc.AddMember("event_id", rapidjson::Value(event_id.c_str(), allocator), allocator);
        doc.AddMember("event_type", rapidjson::Value(event_type.c_str(), allocator), allocator);
        doc.AddMember("channel", rapidjson::Value(channel.c_str(), allocator), allocator);
        doc.AddMember("request_id", rapidjson::Value(request_id.c_str(), allocator), allocator);
        doc.AddMember("data", rapidjson::Value(data_json.c_str(), allocator), allocator);

        // bool 类型
        doc.AddMember("done", done, allocator);

        // int64_t 时间戳（安全转换）
        doc.AddMember("timestamp", static_cast<int64_t>(timestamp), allocator);

        // 生成最终 JSON 字符串
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);

        return buffer.GetString();
    }
}