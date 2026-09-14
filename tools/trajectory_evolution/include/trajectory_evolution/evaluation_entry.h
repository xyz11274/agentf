#pragma once
#ifdef __cplusplus
extern "C" {
#endif

/* A borrowed JSON input, valid for the duration of the callback. Return zero
 * when accepted, nonzero on failure. Completion comes from normal run-end points,
 * not this return code. No callback is retried. Call on the host's valid thread.
 * Returns 0 outside evaluation, 1 after all turns, -1 on error. */
typedef int (*te_evaluation_receiver)(const char* input_json, void* context);
int te_evaluate(te_evaluation_receiver receiver, void* context);

#ifdef __cplusplus
}
#endif
