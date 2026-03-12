// Copyright (c) 2026 AgentFlow Authors
// Licensed under MIT License
//
// NodeDataBusClientImpl implementation

#include "runtime/databus/node_databus_client.h"

namespace agentflow {
namespace runtime {

namespace {

/// Convert DataBusNamespace enum to string
const char* namespace_to_string(DataBusNamespace ns) {
  switch (ns) {
    case DataBusNamespace::SYS:
      return "sys";
    case DataBusNamespace::CTX:
      return "ctx";
    case DataBusNamespace::LLM:
      return "llm";
    case DataBusNamespace::TOOL:
      return "tool";
    case DataBusNamespace::USER:
      return "user";
    default:
      return "user";
  }
}

}  // anonymous namespace

NodeDataBusClientImpl::NodeDataBusClientImpl(INodeDataBus* databus, const std::string& session_id)
    : databus_(databus), session_id_(session_id) {}

std::string NodeDataBusClientImpl::build_namespace_prefix(DataBusNamespace ns) const {
  return "/session/" + session_id_ + "/" + namespace_to_string(ns) + "/";
}

INamespaceClient* NodeDataBusClientImpl::get_or_create_client(
    std::unique_ptr<NamespaceClientImpl>& client, DataBusNamespace ns) {
  if (!client) {
    client = std::make_unique<NamespaceClientImpl>(databus_, build_namespace_prefix(ns));
  }
  return client.get();
}

INamespaceClient* NodeDataBusClientImpl::sys() {
  return get_or_create_client(sys_client_, DataBusNamespace::SYS);
}

INamespaceClient* NodeDataBusClientImpl::ctx() {
  return get_or_create_client(ctx_client_, DataBusNamespace::CTX);
}

INamespaceClient* NodeDataBusClientImpl::llm() {
  return get_or_create_client(llm_client_, DataBusNamespace::LLM);
}

INamespaceClient* NodeDataBusClientImpl::tool() {
  return get_or_create_client(tool_client_, DataBusNamespace::TOOL);
}

INamespaceClient* NodeDataBusClientImpl::user() {
  return get_or_create_client(user_client_, DataBusNamespace::USER);
}

const std::string& NodeDataBusClientImpl::session_id() const {
  return session_id_;
}

INodeDataBus* NodeDataBusClientImpl::raw() {
  return databus_;
}

}  // namespace runtime
}  // namespace agentflow