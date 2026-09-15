#ifndef TRAJECTORY_EVOLUTION_HTTP_CLIENT_H
#define TRAJECTORY_EVOLUTION_HTTP_CLIENT_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Initialize once before starting client threads. Requires libcurl >= 7.62. */
int te_initialize(void);
void te_cleanup(void);
typedef struct te_client {
  const char *url;                 /* HTTPS origin or loopback HTTP origin. */
  const char *token;               /* Borrowed credential; never part of JSON. */
  long timeout_ms;                /* 1..300000, including transfer time. */
  size_t max_response_bytes;      /* 1024..16777216; typically 2097152. */
  unsigned retries;              /* 0..3, same request bytes only. */
} te_client;

/* request_json includes operation and a durable request_id; use your JSON library.
 * Success returns the full {ok,result} envelope, owned by caller via te_free.
 * Failure returns a fixed error code, never a raw network/HTTP exception body.
 * A shared immutable client supports concurrent calls; outputs are per call. */
int te_invoke(const te_client *client, const char *request_json,
              char **response_json, long *http_status, const char **error_code);
void te_free(void *value);
#ifdef __cplusplus
}
#endif
#endif
