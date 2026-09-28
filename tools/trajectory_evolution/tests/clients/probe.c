// probe 回归验证：检查包内协议、边界条件与失败处理。
// 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
// 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

#include "../../clients/c/evolution.h"
#include <stdio.h>
#include <stdlib.h>

// 回归用例：main。
// 构造本函数中的固定输入并核验状态与返回结果；不发起真实业务实验。
int main(void) {
  if (te_initialize() != 0)
    return 2;
  te_client client = {getenv("EVOLUTION_URL"), getenv("EVOLUTION_TEST_TOKEN"), 7000, 2097152, 0};
  char *line = NULL;
  size_t size = 0;
  while (getline(&line, &size, stdin) >= 0) {
    char *result = NULL;
    const char *code = NULL;
    long status = 0;
    if (te_invoke(&client, line, &result, &status, &code) == 0) {
      puts(result);
      te_free(result);
    } else
      printf("{\"ok\":false,\"error\":\"%s\",\"status\":%ld}\n", code, status);
    fflush(stdout);
  }
  free(line);
  te_cleanup();
  return 0;
}
