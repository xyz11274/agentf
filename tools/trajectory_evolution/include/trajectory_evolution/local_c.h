#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stable UTF-8 JSON C ABI for any language with native interop. Creation and
 * close are startup/shutdown operations. invoke never waits for external I/O.
 * All returned strings belong to this library; release with te_local_free. */
typedef struct te_local_handle te_local_handle;
te_local_handle* te_local_open(const char* config, const char* bearer,
                               const char* options_json, char** error);
char* te_local_invoke(te_local_handle* handle, const char* request, size_t length);
/* Same point envelope for independent processes. timeout_ms=0 selects 20 ms;
 * one attempt, no retry. Failure returns the original input and degraded context.
 * Release the full {ok,result} envelope with te_local_free. */
char* te_remote_point(const char* origin, const char* bearer, const char* request,
                      size_t length, unsigned timeout_ms);
/* Blocking management only; never call on the Agent's execution path. */
char* te_local_manage(te_local_handle* handle, const char* request, size_t length);
char* te_local_metrics(te_local_handle* handle);
void te_local_set_enabled(te_local_handle* handle, int enabled);
void te_local_drain(te_local_handle* handle);
void te_local_close(te_local_handle* handle);
void te_local_free(char* value);

#ifdef __cplusplus
}
#endif
