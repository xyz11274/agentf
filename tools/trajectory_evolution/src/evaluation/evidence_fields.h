// 评测元数据读取：只转换当前判断所需的字段，不反复序列化完整执行轨迹。
#pragma once

#include "integration_json.h"
#include <initializer_list>

namespace trajectory::evolution::integration {

// 缓存、恢复和汇总通常只需要版本、评分或产物索引等少数字段。先从 Protobuf
// 选取这些字段，再沿用原 JSON 转换，避免为了读取一个标志复制整条长对话。
// 缺失字段仍保持缺失，显式 null 和原有类型不变；不改写原始 evidence。
// 这不是脱敏或完整性校验入口，调用方必须保留整份报告原有的校验和落盘流程。
inline Json evidence_fields(const google::protobuf::Struct &evidence,
                            std::initializer_list<const char *> names) {
  google::protobuf::Struct selected;
  for (const auto *name : names) {
    const auto found = evidence.fields().find(name);
    if (found != evidence.fields().end())
      (*selected.mutable_fields())[name] = found->second;
  }
  return json(selected);
}

} // 命名空间结束：trajectory::evolution::integration
