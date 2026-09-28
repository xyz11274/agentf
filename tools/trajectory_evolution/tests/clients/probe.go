// probe 回归验证：检查包内协议、边界条件与失败处理。
// 本地桩用于重现确定性场景，不代表真实 Agent 或远程模型的业务能力。
// 测试应同时验证正常路径与拒绝路径，避免错误数据被当成成功结果。

package main

import (
	"bufio"
	"context"
	"encoding/json"
	evolution "example.org/trajectory-evolution-client"
	"os"
)

// 回归用例：main。
// 重点核验字段或诊断：client_initialization_failed、probe_input_invalid、probe_read_failed；异常路径不能被当成成功。
func main() {
	client, err := evolution.New(os.Getenv("EVOLUTION_URL"), os.Getenv("EVOLUTION_TEST_TOKEN"), evolution.Options{})
	if err != nil {
		panic("client_initialization_failed")
	}
	defer client.Close()
	input := bufio.NewScanner(os.Stdin)
	input.Buffer(make([]byte, 65536), 2097152)
	output := json.NewEncoder(os.Stdout)
	for input.Scan() {
		var request map[string]any
		if json.Unmarshal(input.Bytes(), &request) != nil {
			panic("probe_input_invalid")
		}
		operation, id := request["operation"].(string), request["request_id"].(string)
		delete(request, "operation")
		delete(request, "request_id")
		result, err := client.Invoke(context.Background(), operation, request, id)
		if err != nil {
			failure := err.(*evolution.Error)
			output.Encode(map[string]any{"ok": false, "error": failure.Code, "status": failure.Status})
		} else {
			output.Encode(map[string]any{"ok": true, "result": result})
		}
	}
	if input.Err() != nil {
		panic("probe_read_failed")
	}
}
