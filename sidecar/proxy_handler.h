#ifndef COREMIND_BRIDGE_PROXY_HANDLER_H
#define COREMIND_BRIDGE_PROXY_HANDLER_H

#include <httplib.h>

/// POST /proxy/memory/query — 查询记忆（→ KMMService）
void handle_proxy_memory_query(const httplib::Request& req, httplib::Response& res);

/// POST /proxy/memory/store — 存储记忆（→ KMMService）
void handle_proxy_memory_store(const httplib::Request& req, httplib::Response& res);

/// POST /proxy/rag/search — RAG 检索（→ GaussClient）
void handle_proxy_rag_search(const httplib::Request& req, httplib::Response& res);

/// POST /proxy/context/add — 添加上下文（→ ContextManager）
void handle_proxy_context_add(const httplib::Request& req, httplib::Response& res);

#endif // COREMIND_BRIDGE_PROXY_HANDLER_H
