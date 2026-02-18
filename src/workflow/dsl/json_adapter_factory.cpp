// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// JSON adapter factory and helper implementation

#include "workflow/json_adapter.h"

#ifdef AGENTFLOW_WITH_RAPIDJSON
#include "adapter/dsl/rapidjson_impl.h"
#else
#include "adapter/dsl/stub_impl.h"
#endif

namespace agentflow {
namespace dsl {

// JsonDocumentFactory implementation

std::shared_ptr<IJsonDocument> JsonDocumentFactory::create() {
#ifdef AGENTFLOW_WITH_RAPIDJSON
  return std::make_shared<RapidjsonDocument>();
#else
  return std::make_shared<StubJsonDocument>();
#endif
}

std::string JsonDocumentFactory::implementation_name() {
#ifdef AGENTFLOW_WITH_RAPIDJSON
  return "rapidjson";
#else
  return "stub";
#endif
}

std::string JsonDocumentFactory::implementation_version() {
#ifdef AGENTFLOW_WITH_RAPIDJSON
  return "1.1.0+";  // Rapidjson version
#else
  return "1.0.0-stub";
#endif
}

// JsonHelper implementation

bool JsonHelper::get_bool(std::shared_ptr<const IJsonValue> obj, const std::string& key,
                          bool default_value) {
  if(!obj || !obj->is_object()) {
    return default_value;
  }

  auto member = obj->get_member(key);
  if(!member || !member->is_bool()) {
    return default_value;
  }

  return member->as_bool();
}

int64_t JsonHelper::get_int(std::shared_ptr<const IJsonValue> obj, const std::string& key,
                            int64_t default_value) {
  if(!obj || !obj->is_object()) {
    return default_value;
  }

  auto member = obj->get_member(key);
  if(!member || !member->is_number()) {
    return default_value;
  }

  return member->as_int64();
}

double JsonHelper::get_double(std::shared_ptr<const IJsonValue> obj, const std::string& key,
                              double default_value) {
  if(!obj || !obj->is_object()) {
    return default_value;
  }

  auto member = obj->get_member(key);
  if(!member || !member->is_number()) {
    return default_value;
  }

  return member->as_double();
}

std::string JsonHelper::get_string(std::shared_ptr<const IJsonValue> obj, const std::string& key,
                                   const std::string& default_value) {
  if(!obj || !obj->is_object()) {
    return default_value;
  }

  auto member = obj->get_member(key);
  if(!member || !member->is_string()) {
    return default_value;
  }

  return member->as_string();
}

std::shared_ptr<const IJsonValue> JsonHelper::get_object(std::shared_ptr<const IJsonValue> obj,
                                                         const std::string&                key) {
  if(!obj || !obj->is_object()) {
    return nullptr;
  }

  auto member = obj->get_member(key);
  if(!member || !member->is_object()) {
    return nullptr;
  }

  return member;
}

std::shared_ptr<const IJsonValue> JsonHelper::get_array(std::shared_ptr<const IJsonValue> obj,
                                                        const std::string&                key) {
  if(!obj || !obj->is_object()) {
    return nullptr;
  }

  auto member = obj->get_member(key);
  if(!member || !member->is_array()) {
    return nullptr;
  }

  return member;
}

}  // namespace dsl
}  // namespace agentflow
