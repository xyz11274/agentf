#include "../../clients/c/evolution.h"
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  if(te_initialize() != 0) return 2;
  te_client client = {getenv("EVOLUTION_URL"), getenv("EVOLUTION_TEST_TOKEN"), 7000, 2097152, 0};
  char *line = NULL; size_t size = 0;
  while(getline(&line, &size, stdin) >= 0) {
    char *result = NULL; const char *code = NULL; long status = 0;
    if(te_invoke(&client, line, &result, &status, &code) == 0) { puts(result); te_free(result); }
    else printf("{\"ok\":false,\"error\":\"%s\",\"status\":%ld}\n", code, status);
    fflush(stdout);
  }
  free(line); te_cleanup(); return 0;
}
