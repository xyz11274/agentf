// c 客户端：通过统一协议收集执行事件并接收策略建议。
// 传输与业务执行分离；接收回执不代表 Agent 已完成整个任务。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

#include "evolution.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

typedef struct response_buffer {
  char *data;
  size_t length, limit;
  int exceeded;
} response_buffer;

// 初始化 C 客户端的网络资源，应在启动并发调用之前执行。
int te_initialize(void) {
  return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1;
}

// 释放进程级网络资源，应在所有客户端调用完成后执行。
void te_cleanup(void) {
  curl_global_cleanup();
}

// 释放由本库返回的字符串，不能使用调用方不匹配的分配器。
void te_free(void *value) {
  free(value);
}

// 收集当前缓冲区或候选片段，遵守容量上限和内存所有权要求。
static size_t collect(char *data, size_t size, size_t count, void *context) {
  response_buffer *buffer = (response_buffer *)context;
  size_t length;
  if (count && size > (size_t)-1 / count)
    return 0;
  length = size * count;
  if (length > buffer->limit - buffer->length) {
    buffer->exceeded = 1;
    return 0;
  }
  char *next = (char *)realloc(buffer->data, buffer->length + length + 1);
  if (!next)
    return 0;
  buffer->data = next;
  memcpy(next + buffer->length, data, length);
  buffer->length += length;
  next[buffer->length] = '\0';
  return length;
}

// 校验服务地址只包含允许的源地址，防止令牌被发给重定向或错误目标。
static int valid_origin(CURLU *url, const char *origin) {
  char *scheme = NULL, *host = NULL, *path = NULL, *extra = NULL;
  int valid = 0;
  if (curl_url_set(url, CURLUPART_URL, origin, 0) != CURLUE_OK)
    goto done;
  if (curl_url_get(url, CURLUPART_SCHEME, &scheme, 0) != CURLUE_OK ||
      curl_url_get(url, CURLUPART_HOST, &host, 0) != CURLUE_OK)
    goto done;
  if (strcmp(scheme, "https") &&
      (strcmp(scheme, "http") ||
       (strcmp(host, "localhost") && strcmp(host, "127.0.0.1") && strcmp(host, "[::1]"))))
    goto done;
  if (curl_url_get(url, CURLUPART_PATH, &path, 0) != CURLUE_OK || strcmp(path, "/"))
    goto done;
  {
    CURLUPart parts[] = {CURLUPART_USER, CURLUPART_PASSWORD, CURLUPART_QUERY, CURLUPART_FRAGMENT};
    unsigned i;
    for (i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
      if (curl_url_get(url, parts[i], &extra, 0) == CURLUE_OK)
        goto done;
    }
  }
  valid = 1;
done:
  curl_free(scheme);
  curl_free(host);
  curl_free(path);
  curl_free(extra);
  return valid;
}

// 执行一个带持久 request_id 的有界协议请求；仅重试同一请求，不重试业务回调。
int te_invoke(const te_client *client, const char *request_json, char **response_json,
              long *http_status, const char **error_code) {
  te_client trial;
  const char *trial_url = getenv("TRAJECTORY_EVOLUTION_TRIAL_URL"),
             *trial_token = getenv("TRAJECTORY_EVOLUTION_TRIAL_TOKEN");
  if (client && trial_url && *trial_url && trial_token && *trial_token) {
    trial = *client;
    trial.url = trial_url;
    trial.token = trial_token;
    client = &trial;
  }
  CURLU *url = NULL;
  char *endpoint = NULL, *authorization = NULL;
  struct curl_slist *headers = NULL, *next = NULL;
  size_t body_length, token_length, i;
  unsigned attempt;
  int result = -1;
  if (!response_json || !http_status || !error_code)
    return -1;
  *response_json = NULL;
  *http_status = 0;
  *error_code = "client_invalid";
  if (!client || !client->url || !client->token || !request_json)
    return -1;
  if (client->timeout_ms < 1 || client->timeout_ms > 300000 || client->max_response_bytes < 1024 ||
      client->max_response_bytes > 16777216 || client->retries > 3) {
    *error_code = "client_limits_invalid";
    return -1;
  }
  token_length = strlen(client->token);
  if (token_length < 24 || token_length > 8192) {
    *error_code = "token_invalid";
    return -1;
  }
  for (i = 0; i < token_length; ++i)
    if ((unsigned char)client->token[i] < 33 || (unsigned char)client->token[i] > 126) {
      *error_code = "token_invalid";
      return -1;
    }
  body_length = strlen(request_json);
  if (body_length > 2097152) {
    *error_code = "request_too_large";
    return -1;
  }
  url = curl_url();
  if (!url || !valid_origin(url, client->url)) {
    *error_code = "https_or_loopback_url_required";
    goto done;
  }
  if (curl_url_set(url, CURLUPART_PATH, "/v1/invoke", 0) != CURLUE_OK ||
      curl_url_get(url, CURLUPART_URL, &endpoint, 0) != CURLUE_OK)
    goto done;
  authorization = (char *)malloc(token_length + sizeof("Authorization: Bearer "));
  if (!authorization)
    goto done;
  strcpy(authorization, "Authorization: Bearer ");
  strcat(authorization, client->token);
  headers = curl_slist_append(NULL, authorization);
  if (!headers)
    goto done;
  next = curl_slist_append(headers, "Content-Type: application/json");
  if (!next)
    goto done;
  headers = next;
  next = curl_slist_append(headers, "Expect:");
  if (!next)
    goto done;
  headers = next;
  for (attempt = 0; attempt <= client->retries; ++attempt) {
    CURL *handle = curl_easy_init();
    response_buffer buffer = {NULL, 0, client->max_response_bytes, 0};
    CURLcode code;
    *http_status = 0;
    if (!handle)
      break;
    curl_easy_setopt(handle, CURLOPT_URL, endpoint);
    curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(handle, CURLOPT_PROXY, "");
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, client->timeout_ms);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, client->timeout_ms);
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request_json);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_length);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &buffer);
    code = curl_easy_perform(handle);
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, http_status);
    curl_easy_cleanup(handle);
    if (code == CURLE_OK && *http_status == 200 && buffer.data) {
      *response_json = buffer.data;
      *error_code = NULL;
      result = 0;
      break;
    }
    *error_code = buffer.exceeded ? "response_too_large"
                                  : (code != CURLE_OK ? "observer_unavailable" : "rpc_failed");
    free(buffer.data);
    if (*http_status > 0 && *http_status < 500)
      break;
  }
done:
  free(authorization);
  curl_slist_free_all(headers);
  curl_free(endpoint);
  curl_url_cleanup(url);
  return result;
}
