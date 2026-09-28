// go 客户端：通过统一协议收集执行事件并接收策略建议。
// 原生入口复用包内 C ABI；注意句柄生命周期与跨语言内存释放。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。
// 本文件包含协议验证或接入示例，不作为真实业务效果的评测成绩。

//go:build linux && cgo

package evolution

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
)

// 回归用例：TestLocalSharedLibraryContractAndConcurrency。
// 重点核验字段或诊断：test_integration_service；异常路径不能被当成成功。
func TestLocalSharedLibraryContractAndConcurrency(t *testing.T) {
	build := os.Getenv("TRAJECTORY_EVOLUTION_TEST_BUILD")
	if build == "" {
		t.Skip("set TRAJECTORY_EVOLUTION_TEST_BUILD to run the real shared-library contract")
	}
	library := filepath.Join(build, "libtrajectory_evolution_local.so")
	fixture := filepath.Join(build, "test_integration_service")
	for _, path := range []string{library, fixture} {
		if info, err := os.Stat(path); err != nil || info.IsDir() {
			t.Fatalf("required native test artifact missing: %s", path)
		}
	}
	root := t.TempDir()
	command := exec.Command(fixture, "--fixture", root)
	if output, err := command.CombinedOutput(); err != nil {
		t.Fatalf("fixture creation failed: %v: %s", err, output)
	}
	token := "fixture-go-local-principal-000001"
	t.Setenv("INTEGRATION_TEST_A", token)
	t.Setenv("INTEGRATION_TEST_B", "fixture-go-local-principal-000002")
	background := false
	client, err := OpenLocal(filepath.Join(root, "service.json"), token, LocalOptions{
		LibraryPath: library, Background: &background, MaxPendingRuns: 32,
		ContentionBudgetUS: 10000,
	})
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	ctx := context.Background()
	contract, err := client.Invoke(ctx, "contract.describe", nil, "go-contract")
	if err != nil || contract["transport"] != "local.native.v1" {
		t.Fatalf("unexpected native contract: %#v err=%v", contract, err)
	}

	const workers = 4
	var executions int32
	var wait sync.WaitGroup
	failures := make(chan error, workers)
	for index := 0; index < workers; index++ {
		wait.Add(1)
		go func(index int) {
			defer wait.Done()
			run := fmt.Sprintf("go-local-%d", index)
			ref := map[string]any{"run": run}
			start := client.Point(ctx, map[string]any{
				"phase": "before", "kind": "run", "ref": ref, "level": "L0",
				"input": "并发本地契约",
			}, 0)
			if start["degraded"] == true {
				failures <- fmt.Errorf("%s start degraded: %#v", run, start)
				return
			}
			call, callErr := client.Call(ctx, map[string]any{
				"kind": "custom", "ref": map[string]any{"run": run, "key": "work"},
				"input": map[string]any{"index": index},
			}, func(input any) (any, error) {
				atomic.AddInt32(&executions, 1)
				return input, nil
			}, 0)
			if callErr != nil || !call.Executed {
				failures <- fmt.Errorf("%s call failed: %#v err=%v", run, call, callErr)
				return
			}
			loop := client.CheckLoop(ctx, map[string]any{
				"ref": map[string]any{"run": run, "key": "loop"}, "state": map[string]any{"round": 1},
			}, 0)
			if !loop.Allow {
				failures <- fmt.Errorf("%s loop failed: %#v", run, loop)
				return
			}
			end := client.Point(ctx, map[string]any{
				"phase": "after", "kind": "run", "ref": ref, "output": "done",
			}, 0)
			if end["degraded"] != true && end["capture_complete"] != true {
				failures <- fmt.Errorf("%s end failed: %#v", run, end)
			}
		}(index)
	}
	wait.Wait()
	close(failures)
	for failure := range failures {
		t.Error(failure)
	}
	if got := atomic.LoadInt32(&executions); got != workers {
		t.Fatalf("business calls executed %d times, want %d", got, workers)
	}
	if err := client.Drain(); err != nil {
		t.Fatal(err)
	}
	if _, err := client.Metrics(); err != nil {
		t.Fatalf("native metrics unavailable: %v", err)
	}
	if _, err := client.CaptureGap(ctx, map[string]any{
		"run_id": "missing-go-boundary", "reason": "contract_probe",
	}); err != nil {
		t.Fatal(err)
	}
}
