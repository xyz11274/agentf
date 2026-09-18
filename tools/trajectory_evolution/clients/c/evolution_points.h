#ifndef TRAJECTORY_EVOLUTION_POINTS_CLIENT_H
#define TRAJECTORY_EVOLUTION_POINTS_CLIENT_H

#include "evolution.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct te_local_handle te_local_handle;
typedef struct te_points_handle te_points_handle;

/* A transport returns a borrowed complete {ok,result} envelope. It is consumed
 * before the callback returns. Returning NULL reports observer_unavailable. */
typedef const char* (*te_points_transport)(const char* request_json,
                                          void* context);
/* The business result is borrowed until execute returns. A nonzero return is
 * copied to business_code and is never replaced by an observation failure. */
typedef int (*te_points_execute)(const char* input_json, void* context,
                                 const char** output_json);

typedef struct te_points_call_result {
  char* output_json;
  char* receipt_json;
  int executed;
  int rejected;
  int business_code;
} te_points_call_result;

/* Callback context and local handle must outlive the points handle. The HTTP
 * constructor copies URL/token/limits from the supplied client. */
te_points_handle* te_points_open_callback(te_points_transport transport,
                                          void* context);
te_points_handle* te_points_open_http(const te_client* client);
te_points_handle* te_points_open_local(te_local_handle* local);
void te_points_close(te_points_handle* handle);

/* fields_json is a JSON object without operation/request_id. Point failures are
 * fail-open and return a degraded result. All returned strings use
 * te_points_free. */
int te_points_point(te_points_handle* handle, const char* fields_json,
                    char** result_json, const char** error_code);
int te_points_call(te_points_handle* handle, const char* fields_json,
                   te_points_execute execute, void* context,
                   te_points_call_result* result, const char** error_code);
int te_points_allow_loop(te_points_handle* handle, const char* fields_json,
                         int* allowed, char** receipt_json,
                         const char** error_code);

/* Explicit capture and isolated evaluation entry use the same transport.
 * capture_fields accepts either ref for a point gap, or run_id/reason for the
 * strict capture.gap operation. */
int te_points_capture_gap(te_points_handle* handle,
                          const char* capture_fields_json,
                          char** receipt_json, const char** error_code);
int te_points_evaluation_next(te_points_handle* handle, const char* consumer,
                              size_t index, char** packet_json,
                              const char** error_code);
int te_points_evaluation_fail(te_points_handle* handle, const char* consumer,
                              char** result_json, const char** error_code);

void te_points_call_result_free(te_points_call_result* result);
void te_points_free(void* value);

#ifdef __cplusplus
}
#endif
#endif
